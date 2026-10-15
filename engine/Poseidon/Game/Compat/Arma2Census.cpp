// Asset census for Arma 1/2 missions: what every entity of the running mission became.
//
// An Arma 2 class is either carried by a CWA class (its CWA model, config and behaviour) or
// imported as A1x_<class> with the Arma model on a CWA base. The census lists, once the
// mission runs, each kind of entity with its class, model, LOD count, crew and weapons, so a
// test run says which Arma 2 assets are really in the game and which are stand-ins.
#include <Poseidon/Game/Compat/Arma1Compat.hpp>

#include <Poseidon/AI/AICenter.hpp>
#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/AI/EntityAI.hpp>
#include <Poseidon/Audio/IAudioSystem.hpp>
#include <Poseidon/Audio/SoundScene.hpp>
#include <Poseidon/Audio/SoundTestLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Evaluator/express.hpp>
#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

namespace Poseidon::Arma1Compat
{
namespace
{
// The player's man (which = 0) or the man nearest him (which = 1), alive, within 30 m.
Man* FaceTarget(int which)
{
    Person* player = GWorld ? GWorld->PlayerOn() : nullptr;
    if (which == 0)
        return dyn_cast<Man>(player);
    if (!player)
        return nullptr;
    Man* best = nullptr;
    float bestDistance = 30.0f;
    const auto consider = [&](Entity* e) {
        Man* man = dyn_cast<Man>(e);
        if (!man || man == player || man->IsDammageDestroyed())
            return;
        const float d = man->Position().Distance(player->Position());
        if (d < bestDistance)
        {
            bestDistance = d;
            best = man;
        }
    };
    for (int i = 0; i < GWorld->NVehicles(); i++)
        consider(GWorld->GetVehicle(i));
    for (int i = 0; i < GWorld->NOutVehicles(); i++)
        consider(GWorld->GetOutVehicle(i));
    return best;
}

std::string TypeName(const Entity* e)
{
    const EntityType* t = e ? e->GetNonAIType() : nullptr;
    return t ? std::string((const char*)t->_className) : std::string("?");
}

std::string Describe(Entity* e)
{
    const EntityType* t = e->GetNonAIType();
    std::string line = TypeName(e);
    // [arma] = drawn with an Arma model; [cwa] = a CWA model (a CWA class, or an A1x_
    // class that kept its CWA base's model, as soldiers do without OP_A2_SOLDIERS).
    std::string shapeName = t ? std::string((const char*)t->GetShapeName()) : std::string();
    std::transform(shapeName.begin(), shapeName.end(), shapeName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool cwaModel = shapeName.rfind("data3d\\", 0) == 0 || shapeName.rfind("\\data3d\\", 0) == 0 ||
                          shapeName.empty();
    line += cwaModel ? " [cwa]" : " [arma]";
    if (t)
    {
        const LODShapeWithShadow* shape = t->GetShape();
        line += " model=" + std::string((const char*)t->GetShapeName());
        line += " lods=" + std::to_string(shape ? shape->NLevels() : 0);
    }
    if (const auto* ai = dyn_cast<EntityAI>(e))
    {
        if (ai->NWeaponSystems() > 0)
        {
            line += " weapons=";
            for (int i = 0; i < ai->NWeaponSystems(); i++)
            {
                const WeaponType* w = ai->GetWeaponSystem(i);
                if (i)
                    line += ",";
                line += w ? std::string((const char*)w->GetName()) : std::string("?");
            }
        }
        // magazines by kind: a soldier with a gun and no rounds for it shows here
        if (ai->NMagazines() > 0 && dyn_cast<const Person>(e))
        {
            std::map<std::string, int> mags;
            for (int i = 0; i < ai->NMagazines(); i++)
                if (const Magazine* m = ai->GetMagazine(i); m && m->_type)
                    mags[std::string((const char*)m->_type->GetName())]++;
            line += " mags=";
            bool first = true;
            for (const auto& [name, count] : mags)
            {
                line += (first ? "" : ",") + std::to_string(count) + "x" + name;
                first = false;
            }
        }
        else if (dyn_cast<const Person>(e) && ai->NWeaponSystems() > 0)
            line += " mags=none";
    }
    if (const auto* tr = dyn_cast<Transport>(e))
    {
        const auto crew = [&](const char* seat, const Person* p)
        {
            if (p)
                line += std::string(" ") + seat + "=" + TypeName(p);
        };
        crew("driver", tr->Driver());
        crew("gunner", tr->Gunner());
        crew("commander", tr->Commander());
    }
    return line;
}
} // namespace

void LogAssetCensus()
{
    if (!Active() || !GWorld)
        return;
    std::map<std::string, int> kinds;
    int arma = 0, cwa = 0;
    const auto add = [&](Entity* e)
    {
        if (!e)
            return;
        const std::string line = Describe(e);
        (line.find(" [arma]") != std::string::npos ? arma : cwa)++;
        kinds[line]++;
        if (const auto* tr = dyn_cast<Transport>(e))
            for (Person* p : {tr->Driver(), tr->Gunner(), tr->Commander()})
                if (p)
                {
                    const std::string crew = Describe(p) + " (crew)";
                    (crew.find(" [arma]") != std::string::npos ? arma : cwa)++;
                    kinds[crew]++;
                }
    };
    for (int i = 0; i < GWorld->NVehicles(); i++)
        add(GWorld->GetVehicle(i));
    for (int i = 0; i < GWorld->NOutVehicles(); i++)
        add(GWorld->GetOutVehicle(i));
    LOG_INFO(Mission, "{}: asset census: {} entities with the Arma model, {} with a CWA model, {} kinds", Label(), arma,
             cwa, kinds.size());
    for (const auto& [line, count] : kinds)
        LOG_INFO(Mission, "{}: census {}x {}", Label(), count, line);

    LogPlayerSurroundings();
}

// Where the player stands and what the map has around him: a unit on a ship or a
// roof stands on a map object, and a missing one shows as a soldier on the water.
// Logged when the mission starts and again when the first screenshot is taken, since
// streamed map objects arrive over the first frames.
void LogPlayerSurroundings()
{
    if (!Active() || !GWorld)
        return;
    Object* on = GWorld->CameraOn();
    if (on && GLandscape)
    {
        const Vector3 p = on->Position();
        const float surface = GLandscape->SurfaceY(p.X(), p.Z());
        LOG_INFO(Mission, "{}: census player {} at {:.1f},{:.1f},{:.1f} (surface {:.1f}, above it {:.1f})", Label(),
                 TypeName(dyn_cast<Entity>(on)), p.X(), p.Y(), p.Z(), surface, p.Y() - surface);
        struct Near { int count = 0; float distance = 0; float y = 0; float top = 0; LODShape* shape = nullptr; };
        std::map<std::string, Near> near; // model -> count, nearest one
        const int cx = static_cast<int>(p.X() * InvLandGrid), cz = static_cast<int>(p.Z() * InvLandGrid);
        const int reach = static_cast<int>(150.0f * InvLandGrid) + 1;
        for (int z = cz - reach; z <= cz + reach; z++)
            for (int x = cx - reach; x <= cx + reach; x++)
            {
                if (x < 0 || z < 0 || x >= LandRange || z >= LandRange)
                    continue;
                const ObjectList& list = GLandscape->GetObjects(z, x);
                for (int i = 0; i < list.Size(); i++)
                {
                    const Object* o = list[i];
                    if (!o || !o->GetShape() || dyn_cast<const EntityAI>(o))
                        continue;
                    const float d = o->Position().Distance(p);
                    if (d > 150.0f)
                        continue;
                    auto& entry = near[std::string((const char*)o->GetShape()->Name())];
                    if (entry.count == 0 || d < entry.distance)
                    {
                        entry.distance = d;
                        entry.y = o->Position().Y();
                        entry.top = o->Position().Y() + o->GetShape()->Max().Y();
                        entry.shape = o->GetShape();
                    }
                    entry.count++;
                }
            }
        // every entity within 3 m of the player (a weapon lying at his feet in third person
        // is either one of these or one of his own proxies)
        const auto closeBy = [&](Entity* e) {
            if (!e || e == on)
                return;
            const float d = e->Position().Distance(p);
            if (d < 3.0f)
                LOG_INFO(Mission, "{}: census within 3 m of the player: {} at {:.2f} m (dy {:.2f})", Label(),
                         Describe(e), d, e->Position().Y() - p.Y());
        };
        for (int i = 0; i < GWorld->NVehicles(); i++)
            closeBy(GWorld->GetVehicle(i));
        for (int i = 0; i < GWorld->NOutVehicles(); i++)
            closeBy(GWorld->GetOutVehicle(i));
        // faces: the player and the men within 25 m -- identity, face and the texture the head
        // draws with (an Arma 2 head must wear an Arma 2 face, ca\characters\hhl\...)
        {
            std::vector<std::pair<float, Man*>> men;
            const auto man = [&](Entity* e) {
                if (Man* m = dyn_cast<Man>(e))
                    if (const float d = m->Position().Distance(p); d < 25.0f)
                        men.emplace_back(d, m);
            };
            for (int i = 0; i < GWorld->NVehicles(); i++)
                man(GWorld->GetVehicle(i));
            for (int i = 0; i < GWorld->NOutVehicles(); i++)
                man(GWorld->GetOutVehicle(i));
            std::sort(men.begin(), men.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < men.size() && i < 8; i++)
            {
                Man* m = men[i].second;
                const AIUnitInfo& info = m->GetInfo();
                LOG_INFO(Mission, "{}: census face {} at {:.1f} m: '{}' face '{}' glasses '{}' texture {}", Label(),
                         TypeName(m), men[i].first, (const char*)info._name, (const char*)info._face,
                         (const char*)info._glasses, (const char*)m->GetFaceTextureName());
            }
        }
        // the mission's own objects nearest the player (a watchtower, a vehicle), with what
        // their first level draws with: T07's camouflage net showed its holes black
        {
            std::vector<std::pair<float, Entity*>> objects;
            const auto collect = [&](Entity* e) {
                if (e && e != on && e->GetShape())
                    if (const float d = e->Position().Distance(p); d < 25.0f)
                        objects.emplace_back(d, e);
            };
            for (int i = 0; i < GWorld->NVehicles(); i++)
                collect(GWorld->GetVehicle(i));
            for (int i = 0; i < GWorld->NOutVehicles(); i++)
                collect(GWorld->GetOutVehicle(i));
            std::sort(objects.begin(), objects.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            std::set<std::string> shown;
            for (const auto& [d, e] : objects)
            {
                const std::string model = (const char*)e->GetShape()->Name();
                if (shown.size() >= 6 || !shown.insert(model).second)
                    continue;
                LOG_INFO(Mission, "{}: census object near player {} at {:.1f} m, model {}", Label(), Describe(e), d,
                         model);
                const Shape* lod0 = e->GetShape()->Level(0);
                for (int i = 0; lod0 && i < lod0->NSections() && i < 10; i++)
                {
                    const ShapeSection& sec = lod0->GetSection(i);
                    const Texture* tex = sec.properties.GetTexture();
                    LOG_INFO(Mission, "{}: census   section {}: {} special {:x}{}", Label(), i,
                             tex ? (const char*)tex->GetName() : "(none)", sec.properties.Special(),
                             tex ? (const_cast<Texture*>(tex)->IsTransparent() ? " (texture transparent)"
                                    : const_cast<Texture*>(tex)->IsAlpha()     ? " (texture alpha)"
                                                                               : "")
                                 : "");
                }
            }
        }
        // nearest first: the 40 the player can actually see, not the first 40 by name (a fence
        // 3 m away was cut off behind a page of roads and bushes)
        std::vector<std::pair<std::string, Near>> byDistance(near.begin(), near.end());
        std::sort(byDistance.begin(), byDistance.end(),
                  [](const auto& a, const auto& b) { return a.second.distance < b.second.distance; });
        int listed = 0;
        for (const auto& [model, info] : byDistance)
            if (listed++ < 40)
            {
                LOG_INFO(Mission, "{}: census near player {}x {} (nearest {:.0f} m, at y {:.1f}, top {:.1f})",
                         Label(), info.count, model, info.distance, info.y, info.top);
                // what the nearest dozen draw with: texture and face flags per section of the
                // first level (a chain-link fence drawn as a white wall has lost its alpha)
                const Shape* lod0 = listed <= 12 && info.shape ? info.shape->Level(0) : nullptr;
                for (int i = 0; lod0 && i < lod0->NSections() && i < 8; i++)
                {
                    const ShapeSection& sec = lod0->GetSection(i);
                    const Texture* tex = sec.properties.GetTexture();
                    LOG_INFO(Mission, "{}: census   section {}: {} special {:x}", Label(), i,
                             tex ? (const char*)tex->GetName() : "(none)", sec.properties.Special());
                }
            }
        LOG_INFO(Mission, "{}: census {} map models within 150 m of the player", Label(), near.size());
    }
}

bool AimFaceCamera(int which)
{
    if (!Active() || !GWorld || !GLandscape)
        return false;
    Man* man = FaceTarget(which);
    if (!man)
    {
        LOG_INFO(Mission, "{}: face shot {}: no man to look at", Label(), which);
        return false;
    }
    const Vector3 headModel = man->GetHeadCenter();
    const Vector3 head = headModel.SquareSize() > 0 ? man->PositionModelToWorld(headModel)
                                                    : man->Position() + VUp * 1.6f;
    Vector3 forward = man->Direction();
    forward[1] = 0;
    if (forward.SquareSize() < 1e-6f)
        forward = VForward;
    forward.Normalize();
    const Vector3 eye = head + forward * 0.9f + VUp * 0.03f;
    // script positions are [x, z, height above the surface]
    const auto aboveSurface = [](Vector3Par v) { return v.Y() - GLandscape->SurfaceYAboveWater(v.X(), v.Z()); };
    char script[768];
    snprintf(script, sizeof(script),
             "if (isNil \"OP_faceCam\") then {OP_faceCam = \"camera\" camCreate [0,0,0]}; "
             "OP_faceCam camSetPos [%.3f,%.3f,%.3f]; OP_faceCam camSetTarget [%.3f,%.3f,%.3f]; "
             "OP_faceCam camSetFov 0.3; OP_faceCam cameraEffect [\"internal\",\"back\"]; OP_faceCam camCommit 0;",
             eye.X(), eye.Z(), aboveSurface(eye), head.X(), head.Z(), aboveSurface(head));
    GWorld->GetGameState()->EvaluateMultiple(script);
    const AIUnitInfo& info = man->GetInfo();
    LOG_INFO(Mission, "{}: face shot {}: {} '{}' face '{}' texture {}", Label(), which, TypeName(man),
             (const char*)info._name, (const char*)info._face, (const char*)man->GetFaceTextureName());
    return true;
}

void EndFaceCamera()
{
    if (!Active() || !GWorld)
        return;
    GWorld->GetGameState()->EvaluateMultiple(
        "if (!isNil \"OP_faceCam\") then {OP_faceCam cameraEffect [\"terminate\",\"back\"]; camDestroy OP_faceCam};");
}

namespace
{
const char* UnitStateName(int s)
{
    static const char* names[] = {"Wait", "Init", "Busy", "Completed", "Delay", "InCargo", "Stopping", "Replan",
                                  "Stopped", "Sending"};
    return s >= 0 && s < static_cast<int>(sizeof(names) / sizeof(names[0])) ? names[s] : "?";
}

const char* CommandName(int m)
{
    static const char* names[] = {"none", "Wait", "Attack", "Hide", "Move", "Heal", "Repair", "Refuel", "Rearm",
                                  "Support", "Join", "GetIn", "Fire", "GetOut", "Stop", "Expect", "Action"};
    return m >= 0 && m < static_cast<int>(sizeof(names) / sizeof(names[0])) ? names[m] : "?";
}

// where each unit stood when the test began (by pointer; a unit that dies and is replaced
// in the same memory is a rare false reading in a diagnostic)
std::unordered_map<const void*, Vector3>& StartPositions()
{
    static std::unordered_map<const void*, Vector3> positions;
    return positions;
}

template <class F> void ForEachMan(F&& f)
{
    const auto visit = [&](Entity* e) {
        if (auto* man = dyn_cast<Man>(e))
            f(man, false);
        if (auto* tr = dyn_cast<Transport>(e))
            for (Person* p : {tr->Driver(), tr->Gunner(), tr->Commander()})
                if (auto* crew = dyn_cast<Man>(p))
                    f(crew, true);
    };
    for (int i = 0; i < GWorld->NVehicles(); i++)
        visit(GWorld->GetVehicle(i));
}
} // namespace

// Units that stand still for a whole test run are a failure the screenshots cannot show:
// a soldier who should walk to his waypoint looks the same as one who is told to wait.
// At the start every man's position is taken; at the end each AI soldier on foot is
// counted as moved or not, and for the ones that did not move the log says what his AI
// was doing -- unit state, the group's command, waypoints, the move he is in, disabled AI.
void LogUnitMotion(bool atStart)
{
    if (!Active() || !GWorld)
        return;
    if (atStart)
    {
        StartPositions().clear();
        ForEachMan([](Man* man, bool) { StartPositions()[man] = man->Position(); });
        return;
    }
    int onFoot = 0, moved = 0, crew = 0, dead = 0, players = 0, listed = 0;
    std::map<std::string, int> stillBy; // why-key -> count
    const Object* player = GWorld->GetRealPlayer();
    ForEachMan([&](Man* man, bool inVehicle) {
        if (inVehicle)
        {
            crew++;
            return;
        }
        if (man == player)
        {
            players++;
            return;
        }
        if (man->IsDammageDestroyed())
        {
            dead++;
            return;
        }
        onFoot++;
        const auto it = StartPositions().find(man);
        const float d = it == StartPositions().end() ? -1.0f : it->second.Distance(man->Position());
        if (d > 2.0f)
        {
            moved++;
            return;
        }
        AIUnit* unit = man->Brain();
        AIGroup* group = unit ? unit->GetGroup() : nullptr;
        AISubgroup* sub = unit ? unit->GetSubgroup() : nullptr;
        const Command* cmd = sub ? sub->GetCommand() : nullptr;
        const int disabled = unit ? unit->GetAIDisabled() : 0;
        const std::string key = std::string(unit ? UnitStateName(unit->GetState()) : "no-brain") + " cmd=" +
                                (cmd ? CommandName(static_cast<int>(cmd->_message)) : "none") +
                                " wps=" + std::to_string(group ? group->NWaypoints() : -1) +
                                (disabled & AIUnit::DAMove ? " disableAI-MOVE" : "") +
                                (unit && unit->IsSubgroupLeader() ? " leader" : "");
        stillBy[key]++;
        if (listed++ < 12)
        {
            LOG_INFO(Mission,
                     "{}: motion still {} moved {:.1f} m: {} move={} speed {:.2f} path={} mode={} planning={} "
                     "disabledAI={}",
                     Label(), TypeName(man), d, key, (const char*)man->GetPrimaryMoveName(),
                     man->Speed().Size(), unit ? unit->GetPath().Size() : -1, unit ? int(unit->GetMode()) : -1,
                     unit ? int(unit->GetPlanningMode()) : -1, disabled);
        }
    });
    LOG_INFO(Mission, "{}: motion AI soldiers on foot {} moved {} still {}; crew {}, dead {}, player {}", Label(), onFoot,
             moved, onFoot - moved, crew, dead, players);
    for (const auto& [key, count] : stillBy)
        LOG_INFO(Mission, "{}: motion still {}x {}", Label(), count, key);
    int vehicles = 0, vehiclesMoved = 0;
    for (int i = 0; i < GWorld->NVehicles(); i++)
        if (auto* tr = dyn_cast<Transport>(GWorld->GetVehicle(i)); tr && tr->Driver() && tr->Driver() != player)
        {
            vehicles++;
            if (tr->Speed().Size() > 0.5f)
                vehiclesMoved++;
        }
    LOG_INFO(Mission, "{}: motion vehicles with an AI driver {}, moving at the end {}", Label(), vehicles,
             vehiclesMoved);
}

// What the game asked the sound system for during the run (SoundTestLog.hpp).
void LogSoundCensus()
{
    if (!Active())
        return;
    using namespace Audio::SoundTestLog;
    std::vector<std::pair<std::string, Entry>> entries;
    {
        std::lock_guard<std::mutex> lock(Mutex());
        entries.assign(Entries().begin(), Entries().end());
    }
    int requested = 0, failed = 0, played = 0;
    for (const auto& [name, e] : entries)
    {
        requested += e.requested;
        failed += e.failed;
        played += e.played;
    }
    LOG_INFO(Mission,
             "{}: sound census {} distinct files, {} requests, {} failed, {} plays, {} empty names; volumes fx {:.2f} "
             "speech {:.2f} music {:.2f}",
             Label(), entries.size(), requested, failed, played, EmptyRequests().load(),
             GSoundsys ? GSoundsys->GetWaveVolume() : -1.0f, GSoundsys ? GSoundsys->GetSpeechVolume() : -1.0f,
             GSoundsys ? GSoundsys->GetCDVolume() : -1.0f);
    if (GSoundsys)
    {
        const AudioCounters c = GSoundsys->GetCounters();
        LOG_INFO(Mission,
                 "{}: sound playing at the end: 2D {} 3D {} paused {} voices {} alloc failures {}; fadeSound {:.2f} "
                 "fadeMusic {:.2f}",
                 Label(), c.active2D, c.active3D, c.paused, c.liveVoices, c.allocFailures,
                 GSoundScene ? GSoundScene->GetSoundVolume() : -1.0f,
                 GSoundScene ? GSoundScene->GetMusicVolume() : -1.0f);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.second.requested > b.second.requested; });
    int listed = 0;
    for (const auto& [name, e] : entries)
        if (listed++ < 40)
            LOG_INFO(Mission, "{}: sound {} requested {} failed {} played {}", Label(), name, e.requested, e.failed,
                     e.played);
    listed = 0;
    for (const auto& [name, e] : entries)
        if (e.failed > 0 && listed++ < 40)
            LOG_INFO(Mission, "{}: sound failed {} ({}x)", Label(), name, e.failed);
}
// Test runs: a few script values at the end, to see where a mission's scripted opening stood
// (Arma 3: did the title card finish, did the establishing shot start the mission). More
// expressions with OP_A3_PROBE="expr;;expr".
void LogScriptProbe()
{
    if (Generation() != 3 || !GWorld || !GWorld->GetGameState())
        return;
    std::vector<std::string> exprs = {"isNil 'BIS_fnc_titlecard_finished'", "isNil 'BIS_missionStarted'",
                                      "isNil 'BIS_fnc_establishingShot_skip'", "str allCutLayers"};
    if (const char* extra = std::getenv("OP_A3_PROBE"))
    {
        std::string all = extra;
        for (size_t start = 0; start < all.size();)
        {
            size_t end = all.find(";;", start);
            if (end == std::string::npos)
                end = all.size();
            if (end > start)
                exprs.push_back(all.substr(start, end - start));
            start = end + 2;
        }
    }
    GameState* gs = GWorld->GetGameState();
    for (const std::string& e : exprs)
    {
        GameVarSpace local(gs->GetContext());
        gs->BeginContext(&local);
        const GameValue v = gs->EvaluateMultiple(e.c_str());
        gs->EndContext();
        LOG_INFO(Mission, "{}: probe {} = {}", Label(), e, (const char*)v.GetText());
    }
}
} // namespace Poseidon::Arma1Compat
