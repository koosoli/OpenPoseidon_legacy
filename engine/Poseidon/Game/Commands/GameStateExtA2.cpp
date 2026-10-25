// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arma 2 script commands (--arma2).
//
// Original Arma 2 missions are written against a much larger scripting surface than
// OFP/CWA offers. This file adds the parts the official Arma 2 / OA missions and their
// shared scripts (MP framework, modules) lean on, implemented on CWA's own systems:
//
//   - variables on objects and groups, and the mission/ui namespaces
//     (setVariable / getVariable / missionNamespace / uiNamespace)
//   - simple tasks and diary records (kept, reported as hints; there is no Arma 2
//     diary UI in CWA)
//   - the conversation system: kbAddTopic reads the .bikb sentence file, kbTell shows
//     the sentence as chat text and plays its recorded speech, kbWasSaid reports it
//     once the line has been spoken
//   - scripted FSMs: execFSM runs an Arma 2 .fsm (the FSM editor's compiled config)
//     state by state, one transition per simulation step, with its own local variables
//   - post-process effects, loading screens and other presentation-only commands are
//     accepted without effect (CWA has no such pipeline)
//
// Everything is ordinary OFP script machinery underneath: code is a string, a task or
// diary record is a tagged string ("::TASK::3"), an FSM handle a number.

#include <set>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/World/Scene/ScriptLight.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/Game/Scripting/Scripts.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/Input/KeyInput.hpp>
#include <Poseidon/UI/DisplayUI.hpp>
#include <Poseidon/UI/Arma3Menu.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/AICenter.hpp>
#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/Network/NetworkIface.hpp>
#include <Poseidon/Audio/DynSound.hpp>
#include <Poseidon/Audio/Streaming/WaveStream.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Foundation/Platform/CrashHandler.hpp>
#include <Poseidon/Foundation/Modules/Modules.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Detection/Detector.hpp>
#include <Poseidon/World/Entities/Vehicles/Transport.hpp>

#include <Random/randomGen.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <Poseidon/Game/Commands/GameStateExtA3Rsc.hpp>
#include <unordered_map>
#include <vector>

// The cut effects without the Arma 3 display layer handling (GameStateExtUi.cpp).
GameValue CutTextEffect(const GameState* state, GameValuePar oper1);
GameValue CutRscEffect(const GameState* state, GameValuePar oper1);

namespace Poseidon
{
extern Input GInput; // InputSubsystem.cpp
}
GameValue VehDoMove(const GameState* state, GameValuePar oper1, GameValuePar oper2); // GameStateExtGrp.cpp
GameValue VehDone(const GameState* state, GameValuePar oper1);                        // GameStateExtGrp.cpp

using namespace Poseidon;

bool PreprocessScriptFile(const char* name, std::string& out); // GameStateExtWorldConfig.cpp
bool A1ConfigIsNull(GameValuePar v);                           // GameStateExtA1Config.cpp
bool Arma3ResolveUiVariable(const char* lowerName, GameValue& value); // GameStateExtA3Functions.cpp

void Arma2RuntimeSimulate(); // called from World::SimulateScripts

// CWA commands reused below (defined in the other GameStateExt*.cpp files).
GameValue ShowHint(const GameState* state, GameValuePar oper1);
GameValue MarkerGetPos(const GameState* state, GameValuePar oper1);
GameValue ObjGroupChat(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSay(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjPlayMove(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjGetPrimaryWeapon(const GameState* state, GameValuePar oper1);
GameValue ObjCommander(const GameState* state, GameValuePar oper1);
GameValue ObjSetPos(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjLock(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1NearObjects(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerCreate(const GameState* state, GameValuePar oper1);
GameValue MarkerDelete(const GameState* state, GameValuePar oper1);
GameValue MarkerGetDir(const GameState* state, GameValuePar oper1);
GameValue MarkerSetPos(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetType(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetSize(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetColor(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetText(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetShape(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetBrush(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue MarkerSetDir(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GameTime(const GameState* state);
GameValue VehCreate(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjMoveInCargo(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue CutText(const GameState* state, GameValuePar oper1);
GameValue CutRsc(const GameState* state, GameValuePar oper1);
GameValue CutObj(const GameState* state, GameValuePar oper1);
GameValue StrFormat(const GameState* state, GameValuePar oper1);
GameValue ObjSide(const GameState* state, GameValuePar oper1);
GameValue A1MissionConfigFile(const GameState* state);
GameValue VehDelete(const GameState* state, GameValuePar oper1);
GameValue Player(const GameState* state);
GameValue ObjSetPosASL(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetVelocity(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue ObjSetVectorDirectionAndUp(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GrpKnowsAbout(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue GroupCreate(const GameState* state, GameValuePar oper1);
GameValue SideCivilian(const GameState* state);
GameValue CenterCreate(const GameState* state, GameValuePar oper1); // GameStateExtWorldConfig.cpp
GameValue ListJoin(const GameState* state, GameValuePar oper1, GameValuePar oper2);
GameValue A1UnitCreate(const GameState* state, GameValuePar oper1, GameValuePar oper2);

GameValue VehProcessAction(const GameState* state, GameValuePar oper1, GameValuePar oper2); // GameStateExtGrp.cpp
GameValue ObjVehicle(const GameState* state, GameValuePar oper1);                         // GameStateExtUi.cpp
GameValue PlaySound(const GameState* state, GameValuePar oper1);                          // GameStateExtUi.cpp

namespace
{
float Now()
{
    return Glob.time - Poseidon::Foundation::Time(0);
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "$STR_x" / "@STR_x" -> localized text; anything else verbatim.
RString LocalizeText(const char* text)
{
    if (!text)
        return RString();
    if (text[0] == '$' || text[0] == '@')
        return LocalizeString(text + 1);
    return RString(text);
}

// ---------------------------------------------------------------------------
// Per-mission state. Reset whenever a new mission starts (time runs backwards or the
// mission file changes).
// ---------------------------------------------------------------------------
struct Fsm
{
    int handle = 0;
    std::string file;
    std::unique_ptr<ParamFile> config;
    const ParamEntry* states = nullptr;
    std::string state;
    std::vector<std::string> finals;
    GameVarSpace vars;
    bool done = false;
    bool started = false;
};

struct KbSentence
{
    float start = 0;
    float end = 0;
};

struct Task
{
    std::string name;
    std::string description;
    std::string state = "CREATED";
    GameValue destination;
    OLink<Object> owner;
};

struct Runtime
{
    std::string mission;
    float lastTime = -1;
    // setVariable storage
    std::unordered_map<const void*, std::map<std::string, GameValue>> objectVars;
    std::map<std::string, GameValue> uiVars;
    // tasks / diary
    std::vector<Task> tasks;
    std::map<const void*, int> currentTask;
    int diaryRecords = 0;
    struct DiaryEntry
    {
        std::string subject;
        std::string title;
        std::string text;
    };
    std::vector<DiaryEntry> diary;
    int revision = 0; // bumped whenever the diary or a task changes (briefing refresh)
    // kb: topic -> parsed bikb, per speaker
    std::map<std::string, std::shared_ptr<ParamFile>> bikbFiles;                 // file -> parsed
    std::map<const void*, std::map<std::string, std::string>> topics;            // speaker -> topic -> file
    std::map<std::string, KbSentence> said;                                      // speaker|receiver|topic|id
    int soundSerial = 0;
    // kbAddTopic's reactions of a unit to a line it hears: an FSM (an AI's) and code (the
    // player's conversation menu), per unit and topic
    struct KbReaction
    {
        std::string fsm;
        GameValue code;
    };
    std::map<const void*, std::map<std::string, KbReaction>> reactions;
    // lines told: a speaker says one line after another, and the one he told it to reacts
    // once it has been said
    struct KbLine
    {
        OLink<Object> who;
        OLink<Object> to;
        std::string topic;
        std::string id;
        std::string wave; // the recording, "" for none
        std::string shown;
        float start = 0;
        float end = 0;
        bool spoken = false;
    };
    std::vector<KbLine> lines;
    std::map<const void*, float> speakingUntil;
    // FSMs
    std::vector<std::unique_ptr<Fsm>> fsms;
    int nextFsm = 0;
    // locations: the world's named places (CfgWorlds >> world >> Names), then any the
    // mission creates
    struct Location
    {
        std::string type;
        std::string text;
        float x = 0;
        float y = 0;
        float sizeA = 0;
        float sizeB = 0;
        bool deleted = false;
    };
    std::vector<Location> locations;
    bool worldLocations = false;
    // setVehicleInit, run by processInitCommands
    std::vector<std::pair<OLink<Object>, std::string>> vehicleInits;
    // attachTo: object follows target at a model-space offset
    struct Attachment
    {
        OLink<Object> object;
        OLink<Object> target;
        Vector3 offset;
    };
    std::vector<Attachment> attachments;
    // mission.sqm objects by id, and the ids each one is synchronized with
    std::map<int, OLink<Object>> missionObjects;
    std::vector<std::pair<int, std::vector<int>>> missionSync;
    // addAction arguments (Arma 2 form), by entity and action id
    std::map<std::pair<const void*, int>, GameValue> actionArguments;
};

Runtime& RT()
{
    static Runtime rt;
    const float now = Now();
    const std::string mission = (const char*)Glob.header.filenameReal;
    if (mission != rt.mission || now + 0.001f < rt.lastTime)
    {
        if (!rt.mission.empty())
            LOG_INFO(Mission, "Arma 2: mission state reset ('{}' t={:.2f} -> '{}' t={:.2f})", rt.mission, rt.lastTime,
                     mission, now);
        rt = Runtime();
        rt.mission = mission;
    }
    rt.lastTime = now;
    return rt;
}

// ---------------------------------------------------------------------------
// Namespaces and variables
// ---------------------------------------------------------------------------
constexpr const char* kNsMission = "::NS::mission";
constexpr const char* kNsUi = "::NS::ui";

const void* VarKey(GameValuePar v)
{
    if (v.GetType() == GameObject)
        return GetObject(v);
    if (v.GetType() == GameGroup)
        return GetGroup(v);
    return nullptr;
}

GameValue A2MissionNamespace(const GameState*)
{
    return GameValue(RString(kNsMission));
}

GameValue A2UiNamespace(const GameState*)
{
    return GameValue(RString(kNsUi));
}

GameValue A2SetVariable(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& args = oper2;
    if (args.Size() < 2 || args[0].GetType() != GameString)
        return NOTHING;
    const RString name = args[0];
    if (oper1.GetType() == GameString)
    {
        const RString ns = oper1;
        if (strcmp(ns, kNsUi) == 0)
            RT().uiVars[Lower((const char*)name)] = args[1];
        else
            const_cast<GameState*>(state)->VarSet(name, args[1]);
        return NOTHING;
    }
    const void* key = VarKey(oper1);
    if (key)
        RT().objectVars[key][Lower((const char*)name)] = args[1];
    return NOTHING;
}

GameValue A2GetVariable(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    RString name;
    GameValue fallback;
    if (oper2.GetType() == GameArray)
    {
        const GameArrayType& args = oper2;
        if (args.Size() < 1 || args[0].GetType() != GameString)
            return NOTHING;
        name = args[0];
        if (args.Size() > 1)
            fallback = args[1];
    }
    else
    {
        name = oper2;
    }
    if (oper1.GetType() == GameString)
    {
        const RString ns = oper1;
        if (strcmp(ns, kNsUi) == 0)
        {
            auto& vars = RT().uiVars;
            const std::string key = Lower((const char*)name);
            const auto it = vars.find(key);
            if (it != vars.end())
                return it->second;
            // Arma 3 keeps its function library in uiNamespace as well
            GameValue function;
            if (Arma3ResolveUiVariable(key.c_str(), function))
            {
                vars[key] = function;
                return function;
            }
            return fallback;
        }
        GameValue v = state->VarGet(name);
        return v.GetNil() ? fallback : v;
    }
    const void* key = VarKey(oper1);
    if (!key)
        return fallback;
    auto& all = RT().objectVars;
    const auto obj = all.find(key);
    if (obj == all.end())
        return fallback;
    const auto it = obj->second.find(Lower((const char*)name));
    return it != obj->second.end() ? it->second : fallback;
}

// ---------------------------------------------------------------------------
// Tasks and diary
// ---------------------------------------------------------------------------
constexpr const char* kTask = "::TASK::";

Task* FindTask(GameValuePar v)
{
    if (v.GetType() != GameString)
        return nullptr;
    const RString s = v;
    if (strncmp(s, kTask, strlen(kTask)) != 0)
        return nullptr;
    const int index = atoi((const char*)s + strlen(kTask)) - 1;
    auto& tasks = RT().tasks;
    return index >= 0 && index < static_cast<int>(tasks.size()) ? &tasks[index] : nullptr;
}

GameValue TaskValue(int index)
{
    return GameValue(RString((std::string(kTask) + std::to_string(index + 1)).c_str()));
}

GameValue A2CreateSimpleTask(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& args = oper2;
    Task task;
    if (args.Size() > 0 && args[0].GetType() == GameString)
        task.name = (const char*)RString(args[0]);
    task.owner = GetObject(oper1);
    auto& tasks = RT().tasks;
    tasks.push_back(task);
    RT().revision++;
    LOG_INFO(Mission, "Arma 2: task '{}' created", task.name);
    return TaskValue(static_cast<int>(tasks.size()) - 1);
}

GameValue A2SetSimpleTaskDescription(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    if (Task* task = FindTask(oper1))
    {
        const GameArrayType& args = oper2;
        if (args.Size() > 0 && args[0].GetType() == GameString)
            task->description = (const char*)RString(args[0]);
        if (args.Size() > 1 && args[1].GetType() == GameString && RString(args[1]).GetLength() > 0)
            task->name = (const char*)RString(args[1]);
        RT().revision++;
    }
    return NOTHING;
}

GameValue A2SetSimpleTaskDestination(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    if (Task* task = FindTask(oper1))
        task->destination = oper2;
    return NOTHING;
}

GameValue A2SetTaskState(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Task* task = FindTask(oper1);
    if (!task)
        return NOTHING;
    const std::string next = Lower((const char*)RString(oper2));
    std::string upper = next;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (upper == task->state)
        return NOTHING;
    task->state = upper;
    RT().revision++;
    LOG_INFO(Mission, "Arma 2: task '{}' -> {}", task->name, upper);
    // Arma 2 shows nothing of its own when a task changes state; missions that want a hint
    // call BIS_fnc_taskHint (or hint) themselves.
    return NOTHING;
}

GameValue A2TaskState(const GameState*, GameValuePar oper1)
{
    Task* task = FindTask(oper1);
    return GameValue(RString(task ? task->state.c_str() : "NONE"));
}

GameValue A2TaskCompleted(const GameState*, GameValuePar oper1)
{
    Task* task = FindTask(oper1);
    return GameValue(task && (task->state == "SUCCEEDED" || task->state == "FAILED" || task->state == "CANCELED"));
}

GameValue A2TaskDescription(const GameState* state, GameValuePar oper1)
{
    Task* task = FindTask(oper1);
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& array = result;
    array.Add(GameValue(RString(task ? task->description.c_str() : "")));
    array.Add(GameValue(RString(task ? task->name.c_str() : "")));
    array.Add(GameValue(RString("")));
    return result;
}

GameValue A2TaskDestination(const GameState* state, GameValuePar oper1)
{
    Task* task = FindTask(oper1);
    if (task && task->destination.GetType() == GameArray)
        return task->destination;
    // a task without a destination is at [0,0,0] (the task handlers measure distances to it)
    GameValue result = state->CreateGameValue(GameArray);
    if (task)
    {
        GameArrayType& p = result;
        p.Add(GameValue(0.0f));
        p.Add(GameValue(0.0f));
        p.Add(GameValue(0.0f));
    }
    return result;
}

GameValue A2SetCurrentTask(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Object* obj = GetObject(oper1);
    const RString s = oper2.GetType() == GameString ? RString(oper2) : RString();
    if (obj && FindTask(oper2))
    {
        const int index = atoi((const char*)s + strlen(kTask)) - 1;
        if (RT().currentTask[obj] != index + 1)
        {
            RT().currentTask[obj] = index + 1; // no hint: Arma 2 shows none here either
        }
    }
    return NOTHING;
}

GameValue A2CurrentTask(const GameState*, GameValuePar oper1)
{
    Object* obj = GetObject(oper1);
    const auto it = RT().currentTask.find(obj);
    if (it == RT().currentTask.end() || it->second <= 0)
        return GameValue(RString(""));
    return TaskValue(it->second - 1);
}

GameValue A2SimpleTasks(const GameState* state, GameValuePar oper1)
{
    Object* obj = GetObject(oper1);
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& array = result;
    const auto& tasks = RT().tasks;
    for (size_t i = 0; i < tasks.size(); i++)
        if (!obj || tasks[i].owner == obj)
            array.Add(TaskValue(static_cast<int>(i)));
    return result;
}

GameValue A2TaskHint(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& args = oper1;
    if (args.Size() > 0 && args[0].GetType() == GameString)
        ShowHint(state, args[0]);
    return NOTHING;
}

GameValue A2CreateDiaryRecord(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& args = oper2;
    if (args.Size() > 1 && args[1].GetType() == GameArray)
    {
        const GameArrayType& entry = args[1];
        Runtime::DiaryEntry record;
        record.subject = args[0].GetType() == GameString ? (const char*)RString(args[0]) : "Diary";
        if (entry.Size() > 0 && entry[0].GetType() == GameString)
            record.title = (const char*)RString(entry[0]);
        if (entry.Size() > 1 && entry[1].GetType() == GameString)
            record.text = (const char*)RString(entry[1]);
        LOG_INFO(Mission, "Arma 2: diary '{}': {}", record.subject, record.title);
        RT().diary.push_back(std::move(record));
        RT().revision++;
    }
    return GameValue(RString(("::DIARY::" + std::to_string(++RT().diaryRecords)).c_str()));
}

// Drops a trailing SQF "// comment" from a code fragment out[from..] (quotes inside a
// config string are doubled; SQF also uses '...').
void StripSqfLineComment(std::string& out, size_t from)
{
    bool dq = false, sq = false;
    for (size_t i = from; i + 1 < out.size(); i++)
    {
        const char c = out[i];
        if (!sq && c == '"' && out[i + 1] == '"')
        {
            dq = !dq;
            i++;
            continue;
        }
        if (!dq && c == '\'')
        {
            sq = !sq;
            continue;
        }
        if (!dq && !sq && c == '/' && out[i + 1] == '/')
        {
            out.resize(i);
            return;
        }
    }
}

// The FSM editor and conversation files continue a string over lines as
// "line one" \n "line two"; CWA's config parser does not know that form. The pieces are
// joined into one string (a line's trailing // comment dropped, as the line break that
// ended it is gone).
std::string JoinConfigStrings(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    const size_t n = in.size();
    size_t i = 0;
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (i < n)
    {
        const char c = in[i];
        if (c == '/' && i + 1 < n && in[i + 1] == '/')
        {
            while (i < n && in[i] != '\n')
                out.push_back(in[i++]);
            continue;
        }
        if (c == '/' && i + 1 < n && in[i + 1] == '*')
        {
            const size_t end = in.find("*/", i + 2);
            const size_t stop = end == std::string::npos ? n : end + 2;
            out.append(in, i, stop - i);
            i = stop;
            continue;
        }
        if (c == '=')
        {
            // A value that starts on the next line, behind the editor's markers
            // (init = /*%FSM<STATEINIT>*/\n\t"..."): CWA's parser takes the line break as
            // the end of an empty value. Pull the string up to the '='.
            out.push_back(c);
            i++;
            size_t k = i;
            while (k < n)
            {
                if (space(in[k]))
                    k++;
                else if (in[k] == '/' && k + 1 < n && in[k + 1] == '*')
                {
                    const size_t end = in.find("*/", k + 2);
                    k = end == std::string::npos ? n : end + 2;
                }
                else
                    break;
            }
            if (k < n && in[k] == '"' && in.find('\n', i) < k)
            {
                out.push_back(' ');
                i = k;
            }
            continue;
        }
        if (c != '"')
        {
            out.push_back(c);
            i++;
            continue;
        }
        out.push_back('"');
        i++;
        size_t fragment = out.size();
        while (i < n)
        {
            if (in[i] != '"')
            {
                out.push_back(in[i++]);
                continue;
            }
            if (i + 1 < n && in[i + 1] == '"')
            {
                out.append("\"\"");
                i += 2;
                continue;
            }
            size_t j = i + 1;
            while (j < n && space(in[j]))
                j++;
            if (j + 1 < n && in[j] == '\\' && in[j + 1] == 'n')
            {
                size_t k = j + 2;
                while (k < n && space(in[k]))
                    k++;
                if (k < n && in[k] == '"')
                {
                    StripSqfLineComment(out, fragment);
                    out.push_back(' ');
                    fragment = out.size();
                    i = k + 1;
                    continue;
                }
            }
            out.push_back('"');
            i++;
            // The value's ';' may stand on a later line, behind the editor's markers
            // ("..."\n\t/*%FSM</STATEINIT""">*/;, CE7B's listener FSMs). CWA's parser ends a
            // value at the line break and then reads that ';' as an entry name: pull it up.
            size_t k = i;
            while (k < n)
            {
                if (space(in[k]))
                    k++;
                else if (in[k] == '/' && k + 1 < n && in[k + 1] == '*')
                {
                    const size_t end = in.find("*/", k + 2);
                    k = end == std::string::npos ? n : end + 2;
                }
                else
                    break;
            }
            if (k < n && in[k] == ';' && in.find('\n', i) < k)
            {
                out.push_back(';');
                i = k + 1;
            }
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Conversations (kb*)
// ---------------------------------------------------------------------------
std::shared_ptr<ParamFile> LoadBikb(const std::string& file)
{
    auto& cache = RT().bikbFiles;
    const auto it = cache.find(Lower(file));
    if (it != cache.end())
        return it->second;
    std::shared_ptr<ParamFile> cfg;
    std::string text;
    if (PreprocessScriptFile(file.c_str(), text))
    {
        cfg = std::make_shared<ParamFile>();
        text = JoinConfigStrings(text);
        QIStream in(text.data(), static_cast<int>(text.size()));
        cfg->Parse(in);
    }
    else
    {
        LOG_WARN(Mission, "Arma 2: conversation file '{}' not found", file);
    }
    cache[Lower(file)] = cfg;
    return cfg;
}

GameValue A2KbAddTopic(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Object* obj = GetObject(oper1);
    const GameArrayType& args = oper2;
    if (!obj || args.Size() < 2 || args[0].GetType() != GameString || args[1].GetType() != GameString)
        return NOTHING;
    const std::string topic = Lower((const char*)RString(args[0]));
    const std::string file = (const char*)RString(args[1]);
    RT().topics[obj][topic] = file;
    LoadBikb(file);
    // [topic, bikb, AI reaction FSM, player event handler]: the FSM runs when an AI hears a
    // line of the topic, the code when the player does (it builds his conversation menu)
    Runtime::KbReaction reaction;
    if (args.Size() > 2 && args[2].GetType() == GameString)
        reaction.fsm = (const char*)RString(args[2]);
    if (args.Size() > 3 && !args[3].GetNil())
        reaction.code = args[3];
    RT().reactions[obj][topic] = reaction;
    LOG_INFO(Mission, "Arma 2: kbAddTopic '{}' ({}{}{}) for {}", topic, file, reaction.fsm.empty() ? "" : ", fsm ",
             reaction.fsm, (const char*)obj->GetDebugName());
    return NOTHING;
}

GameValue A2KbRemoveTopic(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Object* obj = GetObject(oper1);
    if (obj)
        RT().topics[obj].erase(Lower((const char*)RString(oper2)));
    return NOTHING;
}

GameValue A2KbHasTopic(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Object* obj = GetObject(oper1);
    if (!obj)
        return GameValue(false);
    const auto& t = RT().topics[obj];
    return GameValue(t.find(Lower((const char*)RString(oper2))) != t.end());
}

std::string SaidKey(const void* who, const void* to, const std::string& topic, const std::string& id)
{
    return std::to_string(reinterpret_cast<uintptr_t>(who)) + "|" + std::to_string(reinterpret_cast<uintptr_t>(to)) +
           "|" + Lower(topic) + "|" + Lower(id);
}

// Length in seconds of a recorded sentence (a bank path), or -1 when it cannot be read.
float SpeechSeconds(const char* bankPath)
{
    Ref<WaveStream> stream = SoundLoadFile(bankPath);
    if (!stream)
        return -1.0f;
    WAVEFORMATEX format{};
    stream->GetFormat(format);
    const int bytes = stream->GetUncompressedSize();
    if (format.nAvgBytesPerSec <= 0 || bytes <= 0)
        return -1.0f;
    return static_cast<float>(bytes) / static_cast<float>(format.nAvgBytesPerSec);
}

// A CfgSounds class for a recorded sentence, so `say` plays it through CWA's sound path.
RString SpeechSoundClass(const char* file, const char* text)
{
    ParamEntry* sounds = Pars.FindEntry("CfgSounds");
    if (!sounds || !file || !*file)
        return RString();
    const std::string name = "A2kb_" + std::to_string(++RT().soundSerial);
    const ParamAccessMode access = sounds->GetAccessMode();
    sounds->SetAccessMode(PAReadAndWrite);
    ParamClass* cls = sounds->AddClass(name.c_str());
    sounds->SetAccessMode(access);
    if (!cls)
        return RString();
    cls->Add("name", RString(""));
    if (ParamEntry* sound = cls->AddArray("sound"))
    {
        std::string path = file;
        // CWA prefixes '\' itself for bank paths; keep Arma 2's absolute form.
        sound->AddValue(RStringB(path.c_str()));
        sound->AddValue(1.0f);
        sound->AddValue(1.0f);
    }
    if (ParamEntry* titles = cls->AddArray("titles"))
    {
        titles->AddValue(0.0f);
        titles->AddValue(RStringB(text ? text : ""));
    }
    return RString(name.c_str());
}

// A line starts: its recording is played, its text shown in the group chat.
void KbSpeak(const GameState* state, Runtime::KbLine& line)
{
    line.spoken = true;
    Object* who = line.who;
    if (!who)
        return;
    const GameValue speaker = GameValueExt(who);
    if (!line.wave.empty())
    {
        const RString cls = SpeechSoundClass(line.wave.c_str(), line.shown.c_str());
        if (cls.GetLength() > 0)
            ObjSay(state, speaker, GameValue(cls));
    }
    ObjGroupChat(state, speaker, GameValue(RString(line.shown.c_str())));
}

GameValue A2KbTell(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Object* who = GetObject(oper1);
    const GameArrayType& args = oper2;
    if (!who || args.Size() < 3 || args[1].GetType() != GameString || args[2].GetType() != GameString)
        return NOTHING;
    Object* to = args[0].GetType() == GameObject ? GetObject(args[0]) : nullptr;
    const std::string topic = (const char*)RString(args[1]);
    const std::string id = (const char*)RString(args[2]);
    Runtime& rt = RT();
    std::string file;
    {
        const auto& mine = rt.topics[who];
        const auto it = mine.find(Lower(topic));
        if (it != mine.end())
            file = it->second;
    }
    float duration = 2.0f;
    float spoken = -1.0f; // length of the recorded line, when there is one
    std::string shown;
    std::string wave;
    if (!file.empty())
    {
        if (const std::shared_ptr<ParamFile> cfg = LoadBikb(file))
        {
            const ParamEntry* sentences = cfg->FindEntry("Sentences");
            const ParamEntry* sentence = sentences ? sentences->FindEntry(id.c_str()) : nullptr;
            if (sentence && sentence->IsClass())
            {
                if (const ParamEntry* text = sentence->FindEntry("text"); text && !text->IsClass() && !text->IsArray())
                    shown = (const char*)LocalizeText((const char*)(RStringB)*text);
                if (const ParamEntry* speech = sentence->FindEntry("speech"); speech && speech->IsArray() && speech->GetSize() > 0)
                {
                    std::string bank = (const char*)(RStringB)(*speech)[0];
                    while (!bank.empty() && (bank[0] == '\\' || bank[0] == '/'))
                        bank.erase(0, 1);
                    if (!bank.empty() && QIFStreamB::FileExist(bank.c_str()))
                    {
                        wave = (const char*)(RStringB)(*speech)[0];
                        // kbWasSaid answers once the line has been spoken, and the
                        // conversation FSMs of Arma 2's missions start the next line on it.
                        // The recording's own length is that moment; a length guessed from
                        // the text is not: of the official missions' 4495 recorded lines,
                        // 2032 run more than 1 s past such a guess and 945 end more than
                        // 1 s before it.
                        spoken = SpeechSeconds(bank.c_str());
                        if (spoken > 0.0f)
                            duration = spoken;
                        else
                            duration = std::max(duration, 0.07f * static_cast<float>(shown.size()) + 1.0f);
                    }
                }
            }
        }
    }
    if (file.empty())
        LOG_WARN(Mission, "Arma 2: kbTell '{}' / '{}': speaker {} has no such topic", topic, id,
                 (const char*)who->GetDebugName());
    if (shown.empty())
        shown = id;
    // Without a readable recording the line is shown as text only, for a reading time
    // estimated from its length.
    if (spoken <= 0.0f)
        duration = std::max(duration, 0.06f * static_cast<float>(shown.size()) + 1.0f);

    // One line after another: Arma 2's conversation FSMs tell several lines of one speaker in
    // a single state ("_this kbTell [_from, _topic, ""..._G_0""]; _this kbTell [... _G_1]"),
    // which he says in turn. A line told while he is still speaking starts when he is done.
    Runtime::KbLine line;
    line.who = who;
    line.to = to;
    line.topic = topic;
    line.id = id;
    line.wave = wave;
    line.shown = shown;
    const float now = Now();
    float& busy = rt.speakingUntil[who];
    line.start = std::max(now, busy);
    line.end = line.start + duration;
    busy = line.end;
    LOG_INFO(Mission, "Arma 2: kbTell '{}' / '{}' by {}: {} {:.1f} s{}", topic, id, (const char*)who->GetDebugName(),
             spoken > 0.0f ? "speech" : "text only", duration,
             line.start > now ? " (after his line before, in " + std::to_string(line.start - now).substr(0, 4) + " s)"
                              : std::string());
    if (line.start <= now)
        KbSpeak(state, line);
    KbSentence s;
    s.start = line.start;
    s.end = line.end;
    rt.said[SaidKey(who, to, topic, id)] = s;
    rt.said[SaidKey(who, nullptr, topic, id)] = s;
    rt.lines.push_back(std::move(line));
    return NOTHING;
}

GameValue A2KbWasSaid(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Object* who = GetObject(oper1);
    const GameArrayType& args = oper2;
    if (!who || args.Size() < 3 || args[1].GetType() != GameString || args[2].GetType() != GameString)
        return GameValue(false);
    Object* to = args[0].GetType() == GameObject ? GetObject(args[0]) : nullptr;
    const float maxAge = args.Size() > 3 && args[3].GetType() == GameScalar ? static_cast<float>(args[3]) : 1e9f;
    const auto& said = RT().said;
    auto it = said.find(SaidKey(who, to, (const char*)RString(args[1]), (const char*)RString(args[2])));
    if (it == said.end())
        it = said.find(SaidKey(who, nullptr, (const char*)RString(args[1]), (const char*)RString(args[2])));
    if (it == said.end())
        return GameValue(false);
    const float now = Now();
    return GameValue(now >= it->second.end && now - it->second.end <= maxAge);
}

// ---------------------------------------------------------------------------
// Scripted FSMs (execFSM)
// ---------------------------------------------------------------------------
std::string EntryText(const ParamEntry* cls, const char* key)
{
    const ParamEntry* e = cls ? cls->FindEntry(key) : nullptr;
    if (!e || e->IsClass() || e->IsArray())
        return {};
    return (const char*)(RStringB)*e;
}

void FsmRun(Fsm& fsm, const std::string& code)
{
    if (code.empty())
        return;
    GameState* gstate = GWorld->GetGameState();
    gstate->BeginContext(&fsm.vars);
    gstate->EvaluateMultiple(code.c_str());
    gstate->EndContext();
}

bool FsmTrue(Fsm& fsm, const std::string& code)
{
    if (code.empty())
        return true;
    GameState* gstate = GWorld->GetGameState();
    gstate->BeginContext(&fsm.vars);
    const GameValue v = gstate->EvaluateMultiple(code.c_str());
    gstate->EndContext();
    return v.GetType() == GameBool && static_cast<bool>(v);
}

void FsmEnter(Fsm& fsm, const std::string& name)
{
    fsm.state = name;
    const ParamEntry* st = fsm.states ? fsm.states->FindEntry(name.c_str()) : nullptr;
    // which states the conversation FSMs walk through (a listener that never answers)
    // ... and the animals' agent tasks (main.fsm: grazing, wandering, fleeing)
    // (per file, 300 each; the animals' idle loop idleSpecialAnim.fsm left out)
    static std::map<std::string, int> reportedBy;
    int& reported = reportedBy[fsm.file];
    if (reported < 300 && (fsm.file.find("bikb") != std::string::npos ||
                           (fsm.file.find("animals2") != std::string::npos &&
                            Lower(fsm.file).find("idlespecialanim") == std::string::npos)))
    {
        ++reported;
        const std::string init = EntryText(st, "init");
        LOG_INFO(Mission, "Arma 2: FSM {} enters '{}'{} (init {} chars)", fsm.file, name, st ? "" : " (no such state)",
                 init.size());
    }
    FsmRun(fsm, EntryText(st, "init"));
    for (const std::string& f : fsm.finals)
        if (_stricmp(f.c_str(), name.c_str()) == 0)
            fsm.done = true;
    if (!st)
        fsm.done = true;
}

void FsmStep(Fsm& fsm)
{
    GameState::s_breakTarget.clear(); // a breakTo whose scope was never found
    if (fsm.done)
        return;
    if (!fsm.started)
    {
        fsm.started = true;
        const ParamEntry* root = fsm.config ? fsm.config->FindEntry("FSM") : nullptr;
        FsmEnter(fsm, EntryText(root, "initState"));
        return;
    }
    const ParamEntry* st = fsm.states ? fsm.states->FindEntry(fsm.state.c_str()) : nullptr;
    const ParamEntry* links = st ? st->FindEntry("Links") : nullptr;
    if (!links || !links->IsClass())
        return;
    std::vector<std::pair<float, const ParamEntry*>> ordered;
    for (int i = 0; i < links->GetEntryCount(); i++)
    {
        const ParamEntry& l = links->GetEntry(i);
        if (!l.IsClass())
            continue;
        const ParamEntry* p = l.FindEntry("priority");
        ordered.emplace_back(p && !p->IsClass() && !p->IsArray() ? static_cast<float>(*p) : 0.0f, &l);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [priority, link] : ordered)
    {
        (void)priority;
        if (!FsmTrue(fsm, EntryText(link, "condition")))
            continue;
        FsmRun(fsm, EntryText(link, "action"));
        FsmEnter(fsm, EntryText(link, "to"));
        return;
    }
}

Fsm* FindFsm(GameValuePar v)
{
    if (v.GetType() != GameScalar)
        return nullptr;
    const int handle = static_cast<int>(static_cast<float>(v));
    for (auto& f : RT().fsms)
        if (f->handle == handle)
            return f.get();
    return nullptr;
}

std::unique_ptr<Fsm> LoadFsm(const std::string& file)
{
    auto fsm = std::make_unique<Fsm>();
    fsm->handle = ++RT().nextFsm;
    fsm->file = file;
    std::string text;
    if (!PreprocessScriptFile(file.c_str(), text))
    {
        LOG_WARN(Mission, "Arma 2: FSM '{}' not found", file);
        fsm->done = true;
    }
    else
    {
        fsm->config = std::make_unique<ParamFile>();
        text = JoinConfigStrings(text);
        QIStream in(text.data(), static_cast<int>(text.size()));
        fsm->config->Parse(in);
        const ParamEntry* root = fsm->config->FindEntry("FSM");
        fsm->states = root ? root->FindEntry("States") : nullptr;
        if (const ParamEntry* finals = root ? root->FindEntry("finalStates") : nullptr; finals && finals->IsArray())
            for (int i = 0; i < finals->GetSize(); i++)
                fsm->finals.push_back((const char*)(RStringB)(*finals)[i]);
        if (!fsm->states)
        {
            LOG_WARN(Mission, "Arma 2: '{}' is not a scripted FSM", file);
            fsm->done = true;
        }
    }
    return fsm;
}

// Until 2026-10-24 '<args> execFSM <file>' never ran (an ignoring entry in GameStateExt
// shadowed this one). Running every such FSM at once -- ALICE, SILVIE, the ambient-animal
// and combat modules, SOM, the missions' own -- left 30 of 98 missions with script errors
// from commands not yet implemented (isFlatEmpty, nearObjectsReady, setUnconscious,
// setGroupIcon, ...) and str_EW2 crashing (run 39). Until those are in, only the animals'
// agent tasks (ca\animals2\...) run by default; OP_A2_EXECFSM=all runs every one.
bool A2ExecFsmAllowed(const std::string& file)
{
    static const bool all = [] {
        const char* e = std::getenv("OP_A2_EXECFSM");
        return e && _stricmp(e, "all") == 0;
    }();
    if (all)
        return true;
    // Arma 3's missions are driven by their FSMs (missionFlow.fsm: the story, its conversations
    // and tasks, the orders the squad follows) and its function library runs its own (feedback);
    // without them Showcase Infantry's squad stood and said nothing. They all run.
    if (Arma1Compat::Generation() == 3)
        return true;
    std::string f = Lower(file);
    while (!f.empty() && f[0] == '\\')
        f.erase(0, 1);
    return f.rfind("ca\\animals2\\", 0) == 0;
}

GameValue A2ExecFSM(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    if (!A2ExecFsmAllowed((const char*)RString(oper2)))
    {
        static std::set<std::string> skipped;
        if (skipped.insert(Lower((const char*)RString(oper2))).second)
            LOG_INFO(Mission, "Arma 2: execFSM {} not started (OP_A2_EXECFSM=all runs it)", (const char*)RString(oper2));
        return NOTHING;
    }
    std::unique_ptr<Fsm> fsm = LoadFsm((const char*)RString(oper2));
    {
        static std::set<std::string> started;
        if (started.insert(fsm->file).second)
            LOG_INFO(Mission, "Arma 2: execFSM starts {}{}", fsm->file, fsm->done ? " (not loaded)" : "");
    }
    fsm->vars.VarSet("_this", oper1.GetNil() ? GameValue() : oper1, true);
    // An agent task's FSM (an Arma 2 animal's main.fsm) knows its unit as _agent; Arma 2's
    // engine starts it so, here it is started by execFSM with the unit as _this.
    if (oper1.GetType() == GameObject)
        fsm->vars.VarSet("_agent", oper1, true);
    const int handle = fsm->handle;
    RT().fsms.push_back(std::move(fsm));
    return GameValue(static_cast<float>(handle));
}

// A line has been said: the one it was told to reacts -- an AI by the topic's FSM, the
// player by its event handler (Arma 2 builds his conversation menu from what it returns;
// there is no such menu here, so only what the handler does itself happens).
void KbReact(const Runtime::KbLine& line)
{
    Object* who = line.who;
    Object* to = line.to;
    if (!who || !to || who == to)
        return;
    Runtime& rt = RT();
    const auto unit = rt.reactions.find(to);
    if (unit == rt.reactions.end())
        return;
    const auto topic = unit->second.find(Lower(line.topic));
    if (topic == unit->second.end())
        return;
    const Runtime::KbReaction reaction = topic->second; // the reaction may add topics
    const bool player = GWorld && to == static_cast<Object*>(GWorld->PlayerOn());
    const auto setVars = [&](GameVarSpace& vars) {
        vars.VarSet("_this", GameValueExt(to), true);
        vars.VarSet("_from", GameValueExt(who), true);
        // GameVarSpace keys are lower case (GameState::VarGet lowers the name it seeks):
        // a "_sentenceId" key is never found, and every listener FSM took its fallback link.
        vars.VarSet("_sentenceid", GameValue(RString(line.id.c_str())), true);
        vars.VarSet("_topic", GameValue(RString(line.topic.c_str())), true);
    };
    if (!player && !reaction.fsm.empty())
    {
        std::unique_ptr<Fsm> fsm = LoadFsm(reaction.fsm);
        setVars(fsm->vars);
        LOG_INFO(Mission, "Arma 2: kb '{}' / '{}' reaches {}: FSM {}", line.topic, line.id,
                 (const char*)to->GetDebugName(), reaction.fsm);
        rt.fsms.push_back(std::move(fsm));
    }
    else if (reaction.code.GetType() == GameString && RString(reaction.code).GetLength() > 0)
    {
        GameVarSpace vars;
        setVars(vars);
        LOG_INFO(Mission, "Arma 2: kb '{}' / '{}' reaches {}{}: event handler", line.topic, line.id,
                 (const char*)to->GetDebugName(), player ? " (player)" : "");
        GameState* gstate = GWorld->GetGameState();
        gstate->BeginContext(&vars);
        gstate->EvaluateMultiple((const char*)RString(reaction.code));
        gstate->EndContext();
    }
}

// Lines whose turn has come are spoken; lines said out reach their listener.
void KbStep(Runtime& rt)
{
    const float now = Now();
    GameState* gstate = GWorld->GetGameState();
    std::vector<Runtime::KbLine> said;
    for (size_t i = 0; i < rt.lines.size();)
    {
        Runtime::KbLine& line = rt.lines[i];
        if (!line.spoken && now >= line.start)
            KbSpeak(gstate, line);
        if (line.spoken && now >= line.end)
        {
            said.push_back(std::move(line));
            rt.lines.erase(rt.lines.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        i++;
    }
    for (const Runtime::KbLine& line : said)
        KbReact(line);
}

// unit moveTo position: Arma 2's low-level move for an agent (an animal's main.fsm walks it
// to a spot it picked), given as a silent doMove; moveToCompleted is true once the unit has
// no move left (unitReady), moveToFailed never.
GameValue A2MoveTo(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    static int logged = 0;
    if (logged < 40)
    {
        Object* obj = GetObject(oper1);
        Vector3 to;
        if (obj && GetPos(state, to, oper2))
        {
            ++logged;
            LOG_INFO(Mission, "Arma 2: moveTo {} from {:.0f},{:.0f} to {:.0f},{:.0f}", (const char*)obj->GetDebugName(),
                     obj->Position().X(), obj->Position().Z(), to.X(), to.Z());
        }
    }
    // An agent (Arma 2's AMBIENT LIFE: the animals of a mission's Vehicles) has no group,
    // and only a unit in a group moves to an order; it gets a civilian group of its own on
    // its first moveTo, as createAgent's agents have (T01's cows stood: "no group").
    if (Object* obj = GetObject(oper1))
    {
        Person* person = dyn_cast<Person>(obj);
        AIUnit* unit = person ? person->Brain() : nullptr;
        if (unit && !unit->GetGroup())
        {
            // join cannot do it: it takes a unit only from the group it already has. The
            // unit is added the way createUnit adds a new one, and the civilian side gets a
            // centre first when the mission has no civilians (T01 has none).
            const GameValue civ = SideCivilian(state);
            CenterCreate(state, civ);
            AIGroup* group = GetGroup(GroupCreate(state, civ));
            static int joinLogged = 0;
            if (group)
            {
                AISubgroup* main = group->MainSubgroup();
                group->AddUnit(unit);
                if (!main && group->MainSubgroup())
                    GetNetworkManager().CreateObject(group->MainSubgroup());
                GetNetworkManager().CreateObject(unit);
                if (!group->Leader() && group->GetCenter())
                    group->GetCenter()->SelectLeader(group);
            }
            if (joinLogged < 5)
            {
                ++joinLogged;
                LOG_INFO(Mission, "Arma 2: moveTo gives agent {} a civilian group: {}", (const char*)obj->GetDebugName(),
                         unit->GetGroup() ? "joined" : "FAILED");
            }
        }
    }
    return ::VehDoMove(state, oper1, oper2);
}

GameValue A2MoveToCompleted(const GameState* state, GameValuePar oper1)
{
    return ::VehDone(state, oper1);
}

GameValue A2MoveToFailed(const GameState*, GameValuePar)
{
    return GameValue(false);
}

GameValue A2ExecFSMUnary(const GameState* state, GameValuePar oper1)
{
    return A2ExecFSM(state, GameValue(), oper1);
}

GameValue A2CompletedFSM(const GameState*, GameValuePar oper1)
{
    Fsm* fsm = FindFsm(oper1);
    return GameValue(!fsm || fsm->done);
}

GameValue A2GetFSMVariable(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Fsm* fsm = FindFsm(oper1);
    GameValue v;
    if (fsm)
        fsm->vars.VarGet(RString(Lower((const char*)RString(oper2)).c_str()), v); // keys are lower case
    return v;
}

// fsm getFSMVariable [name, default] (Arma 3): the default when the FSM or the variable
// is not there -- also when the handle itself is nil (an FSM that never started), which a
// trigger condition polls every frame.
GameValue A3GetFSMVariableDefault(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& args = oper2;
    GameValue fallback = args.Size() >= 2 ? args[1] : GameValue();
    if (args.Size() < 1 || args[0].GetType() != GameString || oper1.GetType() != GameScalar)
        return fallback;
    Fsm* fsm = FindFsm(oper1);
    GameValue v;
    if (fsm && fsm->vars.VarGet(RString(args[0]), v) && !v.GetNil())
        return v;
    return fallback;
}

GameValue A2SetFSMVariable(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Fsm* fsm = FindFsm(oper1);
    const GameArrayType& args = oper2;
    if (fsm && args.Size() >= 2 && args[0].GetType() == GameString)
        fsm->vars.VarSet(RString(Lower((const char*)RString(args[0])).c_str()), args[1]); // as the FSM reads it
    return NOTHING;
}

// ---------------------------------------------------------------------------
// Mission flow, units and small helpers
// ---------------------------------------------------------------------------
GameValue A2EndMission(const GameState*, GameValuePar oper1)
{
    const std::string kind = Lower((const char*)RString(oper1));
    EndMode mode = EMEnd1;
    if (kind.size() == 4 && kind.rfind("end", 0) == 0 && kind[3] >= '1' && kind[3] <= '6')
        mode = static_cast<EndMode>(EMEnd1 + (kind[3] - '1'));
    else if (kind == "loser")
        mode = EMLoser;
    else if (kind == "killed")
        mode = EMKilled;
    LOG_INFO(Mission, "Arma 2: endMission \"{}\"", kind);
    GWorld->SetEndMode(mode);
    return NOTHING;
}

GameValue A2FailMission(const GameState* state, GameValuePar oper1)
{
    GWorld->SetEndMode(EMLoser);
    return NOTHING;
}

void CollectGroups(std::vector<AIGroup*>& out)
{
    AICenter* centers[] = {GWorld->GetWestCenter(), GWorld->GetEastCenter(), GWorld->GetGuerrilaCenter(),
                           GWorld->GetCivilianCenter(), GWorld->GetLogicCenter()};
    for (AICenter* c : centers)
    {
        if (!c)
            continue;
        for (int i = 0; i < c->NGroups(); i++)
            if (AIGroup* g = c->GetGroup(i))
                out.push_back(g);
    }
}

GameValue A2AllUnits(const GameState* state)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& array = result;
    std::vector<AIGroup*> groups;
    CollectGroups(groups);
    for (AIGroup* g : groups)
    {
        if (g->GetCenter() == GWorld->GetLogicCenter())
            continue;
        for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
            if (AIUnit* unit = g->UnitWithID(i + 1))
                if (EntityAI* person = unit->GetPerson())
                    array.Add(GameValueExt(person));
    }
    return result;
}

GameValue A2AllGroups(const GameState* state)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& array = result;
    std::vector<AIGroup*> groups;
    CollectGroups(groups);
    for (AIGroup* g : groups)
        array.Add(GameValueExt(g));
    return result;
}

GameValue A2LifeState(const GameState*, GameValuePar oper1)
{
    Object* obj = GetObject(oper1);
    return GameValue(RString(obj && !obj->IsDammageDestroyed() ? "ALIVE" : "DEAD"));
}

GameValue A2VehicleVarName(const GameState*, GameValuePar oper1)
{
    // the name the mission gave the object (its sqm "text"), else ""
    Object* obj = GetObject(oper1);
    return GameValue(obj ? obj->GetVarName() : RString(""));
}

GameValue A2NearestObjects(const GameState* state, GameValuePar oper1)
{
    // nearestObjects [position, ["Class", ...], radius]
    const GameArrayType& args = oper1;
    GameValue result = state->CreateGameValue(GameArray);
    if (args.Size() < 3)
        return result;
    const float radius = args[2].GetType() == GameScalar ? static_cast<float>(args[2]) : 50.0f;
    const GameArrayType* types = args[1].GetType() == GameArray ? &static_cast<const GameArrayType&>(args[1]) : nullptr;
    if (!types || types->Size() == 0)
        return A1NearObjects(state, args[0], GameValue(radius));
    GameArrayType& out = result;
    std::vector<Object*> seen;
    for (int i = 0; i < types->Size(); i++)
    {
        if ((*types)[i].GetType() != GameString)
            continue;
        GameArrayType query;
        query.Add((*types)[i]);
        query.Add(GameValue(radius));
        const GameValue part = A1NearObjects(state, args[0], GameValue(query));
        const GameArrayType& list = part;
        for (int k = 0; k < list.Size(); k++)
        {
            Object* o = GetObject(list[k]);
            if (o && std::find(seen.begin(), seen.end(), o) == seen.end())
            {
                seen.push_back(o);
                out.Add(list[k]);
            }
        }
    }
    return result;
}

GameValue A2NearEntities(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameValue all = A1NearObjects(state, oper1, oper2);
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& out = result;
    const GameArrayType& list = all;
    for (int i = 0; i < list.Size(); i++)
        if (dyn_cast<EntityAI>(GetObject(list[i])))
            out.Add(list[i]);
    return result;
}

GameValue A2GetPosATL(const GameState* state, GameValuePar oper1)
{
    Object* obj = GetObject(oper1);
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& array = result;
    if (!obj)
        return result;
    const Vector3 pos = obj->Position();
    array.Add(GameValue(pos.X()));
    array.Add(GameValue(pos.Z()));
    array.Add(GameValue(pos.Y() - GLandscape->SurfaceY(pos.X(), pos.Z())));
    return result;
}

GameValue A2DiagTickTime(const GameState*)
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return GameValue(std::chrono::duration<float>(clock::now() - start).count());
}

// the frame rate over the last 16 frames (Arma 3's own window for diag_fps)
GameValue A2DiagFps(const GameState*)
{
    const DWORD ms = GEngine ? GEngine->GetAvgFrameDuration(16) : 0;
    return GameValue(ms > 0 ? 1000.0f / static_cast<float>(ms) : 0.0f);
}

GameValue A2DiagFrameNo(const GameState*)
{
    static float frames = 0;
    return GameValue(frames += 1.0f);
}

GameValue A2False(const GameState*)
{
    return GameValue(false);
}

GameValue A2True(const GameState*)
{
    return GameValue(true);
}

GameValue A2Zero(const GameState*)
{
    return GameValue(0.0f);
}

GameValue A2One(const GameState*)
{
    return GameValue(1.0f);
}

GameValue A2NothingNular(const GameState*)
{
    return NOTHING;
}

GameValue A2NothingUnary(const GameState*, GameValuePar)
{
    return NOTHING;
}

GameValue A2NothingBinary(const GameState*, GameValuePar, GameValuePar)
{
    return NOTHING;
}

GameValue A2TrueUnary(const GameState*, GameValuePar)
{
    return GameValue(true);
}

GameValue A2TrueBinary(const GameState*, GameValuePar, GameValuePar)
{
    return GameValue(true);
}

GameValue A2OneUnary(const GameState*, GameValuePar)
{
    return GameValue(1.0f);
}

GameValue A2ZeroUnary(const GameState*, GameValuePar)
{
    return GameValue(0.0f);
}

GameValue A2EmptyArrayUnary(const GameState* state, GameValuePar)
{
    return state->CreateGameValue(GameArray);
}

// actionKeysNamesArray "action" | ["action", maxKeys]: the names of the keys bound to a
// user action, from the player's own bindings (Arma 2's bootcamp hints print them: "press
// 'W'"). Arma 2 action names are this engine's names for the actions they share
// ("MoveForward", "ReloadMagazine", "Compass", ...); a few are spelt differently.
GameValue A2ActionKeysNamesArray(const GameState* state, GameValuePar oper1)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& names = result;
    RString action;
    int maxKeys = -1;
    if (oper1.GetType() == GameString)
        action = oper1;
    else if (oper1.GetType() == GameArray)
    {
        const GameArrayType& args = oper1;
        if (args.Size() > 0 && args[0].GetType() == GameString)
            action = args[0];
        if (args.Size() > 1 && args[1].GetType() == GameScalar)
            maxKeys = toInt((float)args[1]);
    }
    if (action.GetLength() == 0)
        return result;
    static const struct
    {
        const char* arma2;
        const char* here;
    } aliases[] = {{"DefaultAction", "Fire"}, {"Gear", "Inventory"}, {"ShowMap", "Map"}};
    const char* wanted = action;
    for (const auto& a : aliases)
        if (stricmp(wanted, a.arma2) == 0)
            wanted = a.here;
    const UserActionDesc* desc = InputSubsystem::GetUserActionDesc();
    for (int i = 0; i < UAN; i++)
    {
        if (stricmp(desc[i].name, wanted) != 0)
            continue;
        const KeyList& keys = GInput.userKeys[i];
        for (int k = 0; k < keys.Size() && (maxKeys < 0 || names.Size() < maxKeys); k++)
            if (keys[k] >= 0)
                names.Add(GameValue(GetKeyName(keys[k])));
        break;
    }
    return result;
}

// actionKeysNames: the same names as one string, each quoted, joined by " or " (Arma 2
// returns a string: the bootcamp hints call compile on it).
GameValue A2ActionKeysNames(const GameState* state, GameValuePar oper1)
{
    const GameValue names = A2ActionKeysNamesArray(state, oper1);
    const GameArrayType& list = names;
    std::string out;
    for (int i = 0; i < list.Size(); i++)
    {
        if (i)
            out += " or ";
        out += "\"";
        out += (const char*)RString(list[i]);
        out += "\"";
    }
    return GameValue(RString(out.c_str()));
}

// getGroupIconParams: no group icons are drawn here, so every group has the neutral ones --
// [colour, text, scale, visible] -- rather than an empty array that scripts index into
// ((getGroupIconParams _grp) select 2 is the scale).
GameValue A2GroupIconParams(const GameState* state, GameValuePar)
{
    GameValue value = state->CreateGameValue(GameArray);
    GameArrayType& a = value;
    GameValue colour = state->CreateGameValue(GameArray);
    GameArrayType& c = colour;
    for (int i = 0; i < 4; i++)
        c.Add(GameValue(1.0f));
    a.Add(colour);
    a.Add(GameValue(RString("")));
    a.Add(GameValue(1.0f));
    a.Add(GameValue(true));
    return value;
}

GameValue A2EmptyArrayBinary(const GameState* state, GameValuePar, GameValuePar)
{
    return state->CreateGameValue(GameArray);
}

// ppEffectCreate [name, priority] gives a handle; [[name, priority], ...] an array of them.
GameValue A2PpEffectCreate(const GameState* state, GameValuePar oper1)
{
    static float next = 0;
    const GameArrayType& a = oper1;
    if (a.Size() > 0 && a[0].GetType() == GameArray)
    {
        GameValue value = state->CreateGameValue(GameArray);
        GameArrayType& out = value;
        for (int i = 0; i < a.Size(); i++)
            out.Add(GameValue(next += 1.0f));
        return value;
    }
    return GameValue(next += 1.0f);
}

// x factor [a, b]: where x lies between a and b, clamped to 0..1 (Arma 2 config
// expressions: radio protocol, sound controllers).
GameValue A2Factor(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& range = oper2;
    if (range.Size() < 2 || range[0].GetType() != GameScalar || range[1].GetType() != GameScalar)
        return GameValue(0.0f);
    const float a = range[0], b = range[1], x = oper1;
    if (std::fabs(b - a) < 1e-9f)
        return GameValue(x >= b ? 1.0f : 0.0f);
    return GameValue(std::clamp((x - a) / (b - a), 0.0f, 1.0f));
}

GameValue A2TextLogFormat(const GameState* state, GameValuePar oper1)
{
    const RString text = StrFormat(state, oper1);
    LOG_INFO(Script, "textLogFormat: {}", (const char*)text);
    return NOTHING;
}

GameValue A2Rank(const GameState*, GameValuePar oper1)
{
    static const char* names[] = {"PRIVATE", "CORPORAL", "SERGEANT", "LIEUTENANT", "CAPTAIN", "MAJOR", "COLONEL"};
    Person* person = dyn_cast<Person>(GetObject(oper1));
    if (!person)
        return GameValue(RString(""));
    const int rank = static_cast<int>(person->GetRank()) - static_cast<int>(RankPrivate);
    return GameValue(RString(rank >= 0 && rank < 7 ? names[rank] : "PRIVATE"));
}

GameValue A2GetFriend(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    const GameDataSide* a = dynamic_cast<const GameDataSide*>(oper1.GetData());
    const GameDataSide* b = dynamic_cast<const GameDataSide*>(oper2.GetData());
    if (!a || !b || !GWorld)
        return GameValue(0.0f);
    AICenter* center = GWorld->GetCenter(a->GetSide());
    if (!center || b->GetSide() >= TSideUnknown)
        return GameValue(a->GetSide() == b->GetSide() ? 1.0f : 0.0f);
    return GameValue(center->GetFriendship(b->GetSide()));
}

GameValue A2HintSilent(const GameState* state, GameValuePar oper1)
{
    return ShowHint(state, oper1);
}

GameValue A2SetPosATL(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return ObjSetPos(state, oper1, oper2);
}

GameValue A2PlayMoveNow(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return ObjPlayMove(state, oper1, oper2);
}

GameValue A2SetVehicleLock(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const std::string mode = Lower((const char*)RString(oper2));
    return ObjLock(state, oper1, GameValue(mode == "locked"));
}

GameValue A2PlayerSide(const GameState* state)
{
    return state->Evaluate("side player");
}

GameValue A2SafeZoneW(const GameState*)
{
    return GameValue(1.0f);
}

// safeZoneX/Y/W/H: this engine's screen is 0..1 both ways. While Arma3Menu evaluates one of Arma 3's
// own screens (--arma3) they answer in Arma 3's coordinates instead (Arma3Rsc::UiSafeZone).
GameValue A2SafeZone(char member)
{
    if (!Poseidon::Arma3Rsc::UiCoordinates())
        return GameValue(member == 'w' || member == 'h' ? 1.0f : 0.0f);
    const Poseidon::Arma3Rsc::SafeZone zone = Poseidon::Arma3Rsc::UiSafeZone();
    return GameValue(member == 'x' ? zone.x : member == 'y' ? zone.y : member == 'w' ? zone.w : zone.h);
}

GameValue A2SafeZoneXNular(const GameState*)
{
    return A2SafeZone('x');
}

GameValue A2SafeZoneYNular(const GameState*)
{
    return A2SafeZone('y');
}

GameValue A2SafeZoneWNular(const GameState*)
{
    return A2SafeZone('w');
}

GameValue A2SafeZoneHNular(const GameState*)
{
    return A2SafeZone('h');
}

GameValue A2MinusOne(const GameState*)
{
    return GameValue(-1.0f);
}

// isNull display/control: those are numbers here, -1 for none
GameValue A2IsNullHandle(const GameState*, GameValuePar oper1)
{
    // an Arma 3 stand-in display that has closed is null too (GameStateExtA3Rsc.cpp)
    return GameValue(static_cast<float>(oper1) < 0 || Poseidon::Arma3Rsc::IsClosedDisplay(static_cast<float>(oper1)));
}

// ctrlShown control: a control handle that exists is taken as shown.
GameValue A2CtrlShown(const GameState*, GameValuePar oper1)
{
    if (Poseidon::Arma3Menu::Script::IsControl(static_cast<float>(oper1)))
        return GameValue(Poseidon::Arma3Menu::Script::Shown(toInt(static_cast<float>(oper1))));
    return GameValue(static_cast<float>(oper1) >= 0);
}

// addBackpack / removeBackpack: an Arma 3 man's backpack (Man::SetA3Gear); nothing for others
GameValue A2AddBackpack(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    if (Poseidon::Man* man = dyn_cast<Poseidon::Man>(GetObject(oper1));
        man && Poseidon::Arma1Compat::Generation() == 3 && oper2.GetType() == GameString)
        man->SetA3Gear(Poseidon::ManType::A3GearBackpack, RString(oper2));
    return NOTHING;
}

GameValue A2RemoveBackpack(const GameState*, GameValuePar oper1)
{
    if (Poseidon::Man* man = dyn_cast<Poseidon::Man>(GetObject(oper1)); man && Poseidon::Arma1Compat::Generation() == 3)
        man->SetA3Gear(Poseidon::ManType::A3GearBackpack, RString());
    return NOTHING;
}

// showHUD bool | [hud, info, radar, compass, direction, menu, group, cursors, panels, kills, showIcon3D]:
// the first item (or the bool) shows or hides the in-game HUD
GameValue A2ShowHud(const GameState*, GameValuePar oper1)
{
    bool shown = true;
    if (oper1.GetType() == GameBool)
        shown = oper1;
    else if (oper1.GetType() == GameArray)
    {
        const GameArrayType& a = oper1;
        shown = a.Size() == 0 || a[0].GetType() != GameBool || (bool)a[0];
    }
    Poseidon::Arma3Rsc::SetHudShown(shown);
    return NOTHING;
}

// ctrlFade / ctrlSetScale: an Arma 3 script control's (Arma3Menu::Script); 0 and nothing otherwise
GameValue A2CtrlFade(const GameState*, GameValuePar oper1)
{
    return GameValue(Poseidon::Arma3Menu::Script::Fade(toInt(static_cast<float>(oper1))));
}

GameValue A2CtrlSetScale(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Poseidon::Arma3Menu::Script::SetScale(toInt(static_cast<float>(oper1)), static_cast<float>(oper2));
    return NOTHING;
}

GameValue A2DeleteCollection(const GameState* state, GameValuePar oper1)
{
    return VehDelete(state, oper1);
}

// createAgent [class, position, markers, placement, special]: a unit in a group of its own
GameValue A2CreateAgent(const GameState* state, GameValuePar oper1)
{
    const GameValue group = GroupCreate(state, SideCivilian(state));
    return A1UnitCreate(state, group, oper1);
}

// switchableUnits: in single player the player alone (no team switch here)
GameValue A2SwitchableUnits(const GameState* state)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& a = result;
    const GameValue p = Player(state);
    if (GetObject(p))
        a.Add(p);
    return result;
}

// ---------------------------------------------------------------------------
// String helpers (Operation Arrowhead): toArray, toString, toLower, toUpper, parseNumber
// ---------------------------------------------------------------------------
GameValue A2ToArray(const GameState* state, GameValuePar oper1)
{
    const RString text = oper1;
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& out = result;
    const unsigned char* p = reinterpret_cast<const unsigned char*>((const char*)text);
    while (p && *p)
    {
        unsigned code = *p++;
        int extra = code >= 0xF0 ? 3 : code >= 0xE0 ? 2 : code >= 0xC0 ? 1 : 0;
        if (extra)
            code &= 0x3F >> extra;
        while (extra-- > 0 && (*p & 0xC0) == 0x80)
            code = (code << 6) | (*p++ & 0x3F);
        out.Add(GameValue(static_cast<float>(code)));
    }
    return result;
}

GameValue A2ToString(const GameState*, GameValuePar oper1)
{
    const GameArrayType& in = oper1;
    std::string out;
    for (int i = 0; i < in.Size(); i++)
    {
        if (in[i].GetType() != GameScalar)
            continue;
        const unsigned c = static_cast<unsigned>(static_cast<float>(in[i]));
        if (c == 0)
            continue;
        if (c < 0x80)
            out.push_back(static_cast<char>(c));
        else if (c < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return GameValue(RString(out.c_str()));
}

GameValue A2ToLower(const GameState*, GameValuePar oper1)
{
    return GameValue(RString(Lower((const char*)RString(oper1)).c_str()));
}

GameValue A2ToUpper(const GameState*, GameValuePar oper1)
{
    std::string s = (const char*)RString(oper1);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return GameValue(RString(s.c_str()));
}

GameValue A2ParseNumber(const GameState*, GameValuePar oper1)
{
    return GameValue(static_cast<float>(std::atof((const char*)RString(oper1))));
}

// with namespace do {code}: there is one variable space here, so the code just runs
GameValue A2With(const GameState*, GameValuePar oper1)
{
    return oper1;
}

GameValue A2WithDo(const GameState* state, GameValuePar, GameValuePar oper2)
{
    return state->EvaluateMultiple(RString(oper2));
}

// obj setVelocityTransformation [pos1, pos2, vel1, vel2, dir1, dir2, up1, up2, t]
// (unit-capture playback, BIS_fnc_UnitPlay): the state interpolated at t in 0..1.
GameValue A2SetVelocityTransformation(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& a = oper2;
    if (a.Size() < 9 || a[8].GetType() != GameScalar)
        return NOTHING;
    const float t = std::clamp(static_cast<float>(a[8]), 0.0f, 1.0f);
    auto lerp = [&](int i, int j) {
        GameValue out = state->CreateGameValue(GameArray);
        GameArrayType& o = out;
        if (a[i].GetType() != GameArray || a[j].GetType() != GameArray)
            return GameValue();
        const GameArrayType& p = a[i];
        const GameArrayType& q = a[j];
        for (int k = 0; k < 3 && k < p.Size() && k < q.Size(); k++)
            o.Add(GameValue(static_cast<float>(p[k]) + (static_cast<float>(q[k]) - static_cast<float>(p[k])) * t));
        return out;
    };
    const GameValue pos = lerp(0, 1);
    if (pos.GetType() == GameArray && static_cast<const GameArrayType&>(pos).Size() == 3)
        ObjSetPosASL(state, oper1, pos);
    const GameValue dir = lerp(4, 5);
    const GameValue up = lerp(6, 7);
    if (dir.GetType() == GameArray && up.GetType() == GameArray && static_cast<const GameArrayType&>(dir).Size() == 3 &&
        static_cast<const GameArrayType&>(up).Size() == 3)
    {
        GameArrayType du;
        du.Add(dir);
        du.Add(up);
        ObjSetVectorDirectionAndUp(state, oper1, GameValue(du));
    }
    const GameValue vel = lerp(2, 3);
    if (vel.GetType() == GameArray && static_cast<const GameArrayType&>(vel).Size() == 3)
        ObjSetVelocity(state, oper1, vel);
    return NOTHING;
}

// side knowsAbout target: the best a group of that side knows
GameValue A2SideKnowsAbout(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    std::vector<AIGroup*> groups;
    CollectGroups(groups);
    const TargetSide side = GetSide(oper1);
    float best = 0;
    for (AIGroup* g : groups)
        if (g->GetCenter() && g->GetCenter()->GetSide() == side)
            best = std::max(best, static_cast<float>(GrpKnowsAbout(state, GameValueExt(g), oper2)));
    return GameValue(best);
}

// unit/vehicle weaponDirection "weapon": where that weapon's barrel points, in world space
// ([x, z, y] like every script position). Was the body's direction for every weapon, so a
// traversed turret read straight ahead (a1gunfire). Falls back to the body's direction for
// an unknown weapon.
GameValue A2WeaponDirection(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& a = result;
    Object* obj = GetObject(oper1);
    Vector3 d = obj ? obj->Direction() : Vector3(0, 0, 1);
    if (EntityAI* ai = dyn_cast<EntityAI>(obj))
    {
        const RString name = oper2;
        for (int i = 0; i < ai->NMagazineSlots(); i++)
        {
            const WeaponType* weapon = ai->GetMagazineSlot(i)._weapon;
            if (weapon && Poseidon::Arma1Compat::ScriptWeaponMatches(weapon->GetName(), name))
            {
                d = ai->GetWeaponDirection(i);
                break;
            }
        }
    }
    a.Add(GameValue(d.X()));
    a.Add(GameValue(d.Z()));
    a.Add(GameValue(d.Y()));
    return result;
}

GameValue A2EmptyArrayNular(const GameState* state)
{
    return state->CreateGameValue(GameArray);
}

GameValue A2TextLogEmpty(const GameState*, GameValuePar)
{
    return GameValue(RString(""));
}

GameValue A2ObjNullUnary(const GameState*, GameValuePar)
{
    return GameValueExt((Object*)nullptr);
}


// playSound [name, isSpeech] (Arma 2): the sound name is what counts here.
GameValue A2PlaySoundArray(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& a = oper1;
    if (a.Size() < 1 || a[0].GetType() != GameString)
        return NOTHING;
    return PlaySound(state, a[0]);
}

// moveOut unit: the unit leaves its vehicle (as the "GetOut" action).
GameValue A2MoveOut(const GameState* state, GameValuePar oper1)
{
    const GameValue veh = ObjVehicle(state, oper1);
    if (GetObject(veh) == GetObject(oper1))
        return NOTHING;
    GameValue args = state->CreateGameValue(GameArray);
    GameArrayType& a = args;
    a.Add(GameValue(RString("GetOut")));
    a.Add(veh);
    return VehProcessAction(state, oper1, args);
}

GameValue A2ObjNull(const GameState*)
{
    return OBJECT_NULL;
}

GameValue A2Faction(const GameState*, GameValuePar oper1)
{
    Object* obj = GetObject(oper1);
    Vehicle* veh = dyn_cast<Vehicle>(obj);
    if (!veh)
        return GameValue(RString(""));
    std::string type = (const char*)veh->GetName();
    if (_strnicmp(type.c_str(), "A1x_", 4) == 0)
        type = type.substr(4);
    const ParamEntry* cls = Arma1Compat::FindConfigClass("CfgVehicles", type.c_str());
    return GameValue(RString(EntryText(cls, "faction").c_str()));
}
// ---------------------------------------------------------------------------
// Locations. A location is the tagged string "::LOC::n"; the world's named places are
// read from the Arma 2 CfgWorlds class on first use.
// ---------------------------------------------------------------------------
constexpr const char* LocTag = "::LOC::";

Runtime::Location* LocationOf(GameValuePar v)
{
    if (v.GetType() != GameString)
        return nullptr;
    const RString s = v;
    const size_t n = strlen(LocTag);
    if (strncmp(s, LocTag, n) != 0)
        return nullptr;
    Runtime& rt = RT();
    const int index = atoi((const char*)s + n);
    if (index < 0 || index >= static_cast<int>(rt.locations.size()) || rt.locations[index].deleted)
        return nullptr;
    return &rt.locations[index];
}

GameValue LocationValue(int index)
{
    return GameValue(RString((std::string(LocTag) + std::to_string(index)).c_str()));
}

// locationNull is a location handle that names no location.
GameValue A2LocationNull(const GameState*)
{
    return LocationValue(-1);
}

// isNull on a location handle (other strings are not null).

GameValue A2IsNullLocation(const GameState*, GameValuePar oper1)
{
    const RString s = oper1;
    if (strncmp(s, "::CFG::", 7) == 0 || strncmp(s, "::MCFG::", 8) == 0)
        return GameValue(A1ConfigIsNull(oper1)); // Arma 3: isNull on a config entry
    if (strncmp(s, LocTag, strlen(LocTag)) != 0)
        return GameValue(false);
    return GameValue(LocationOf(oper1) == nullptr);
}

// Weather getters (Arma 1/2): the current values of what setOvercast/setFog/setRain set.
GameValue A2Overcast(const GameState*)
{
    return GameValue(GLandscape ? GLandscape->GetOvercast() : 0.0f);
}

GameValue A2Fog(const GameState*)
{
    return GameValue(GLandscape ? GLandscape->GetFog() : 0.0f);
}

GameValue A2Rain(const GameState*)
{
    return GameValue(GLandscape ? GLandscape->GetRainDensity() : 0.0f);
}

// wind: [x, y, z] in m/s (Arma's x east, y north)
GameValue A2Wind(const GameState* state)
{
    GameValue value = state->CreateGameValue(GameArray);
    GameArrayType& a = value;
    const Vector3 w = GLandscape ? GLandscape->GetWind() : VZero;
    a.Add(GameValue(w.X()));
    a.Add(GameValue(w.Z()));
    a.Add(GameValue(w.Y()));
    return value;
}

// serverCommand: there is no dedicated server to command.
GameValue A2ServerCommand(const GameState*, GameValuePar)
{
    return GameValue(false);
}

// worldName: the world's CfgWorlds class name, spelled as the config spells it.
GameValue A2WorldName(const GameState*)
{
    std::string world = Glob.header.worldname;
    if (_strnicmp(world.c_str(), "A2_", 3) == 0)
        world = world.substr(3);
    if (const ParamEntry* cls = Arma1Compat::FindConfigClass("CfgWorlds", world.c_str()))
        return GameValue(RString(cls->GetName()));
    return GameValue(RString(world.c_str()));
}

void LoadWorldLocations(Runtime& rt)
{
    if (rt.worldLocations)
        return;
    rt.worldLocations = true;
    std::string world = Glob.header.worldname;
    if (_strnicmp(world.c_str(), "A2_", 3) == 0)
        world = world.substr(3);
    const ParamEntry* cls = Arma1Compat::FindConfigClass("CfgWorlds", world.c_str());
    const ParamEntry* names = cls ? cls->FindEntry("Names") : nullptr;
    for (int i = 0; names && names->IsClass() && i < names->GetEntryCount(); i++)
    {
        const ParamEntry& e = names->GetEntry(i);
        if (!e.IsClass())
            continue;
        const ParamEntry* pos = e.FindEntry("position");
        if (!pos || !pos->IsArray() || pos->GetSize() < 2)
            continue;
        Runtime::Location loc;
        loc.type = EntryText(&e, "type");
        loc.text = (const char*)LocalizeText(EntryText(&e, "name").c_str());
        loc.x = static_cast<float>((*pos)[0]);
        loc.y = static_cast<float>((*pos)[1]);
        if (const ParamEntry* a = e.FindEntry("radiusA"); a && !a->IsClass() && !a->IsArray())
            loc.sizeA = static_cast<float>(*a);
        if (const ParamEntry* b = e.FindEntry("radiusB"); b && !b->IsClass() && !b->IsArray())
            loc.sizeB = static_cast<float>(*b);
        rt.locations.push_back(loc);
    }
    LOG_INFO(Mission, "Arma 2: {} named locations on '{}'", rt.locations.size(), world);
}

// A position argument: [x, y(, z)] (y = north), an object, or a location.
bool Position2D(GameValuePar v, float& x, float& y)
{
    if (v.GetType() == GameArray)
    {
        const GameArrayType& a = v;
        if (a.Size() < 2 || a[0].GetType() != GameScalar || a[1].GetType() != GameScalar)
            return false;
        x = a[0];
        y = a[1];
        return true;
    }
    if (const Runtime::Location* loc = LocationOf(v))
    {
        x = loc->x;
        y = loc->y;
        return true;
    }
    if (Object* obj = GetObject(v))
    {
        x = obj->Position().X();
        y = obj->Position().Z();
        return true;
    }
    return false;
}

GameValue A2CreateLocation(const GameState*, GameValuePar oper1)
{
    // createLocation [type, position, sizeX, sizeY]
    const GameArrayType& args = oper1;
    Runtime& rt = RT();
    LoadWorldLocations(rt);
    Runtime::Location loc;
    if (args.Size() > 0 && args[0].GetType() == GameString)
        loc.type = (const char*)(RString)args[0];
    if (args.Size() > 1)
        Position2D(args[1], loc.x, loc.y);
    if (args.Size() > 2 && args[2].GetType() == GameScalar)
        loc.sizeA = args[2];
    if (args.Size() > 3 && args[3].GetType() == GameScalar)
        loc.sizeB = args[3];
    rt.locations.push_back(loc);
    return LocationValue(static_cast<int>(rt.locations.size()) - 1);
}

GameValue A2DeleteLocation(const GameState*, GameValuePar oper1)
{
    if (Runtime::Location* loc = LocationOf(oper1))
        loc->deleted = true;
    return NOTHING;
}

GameValue A2LocSetText(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    if (Runtime::Location* loc = LocationOf(oper1))
        loc->text = (const char*)(RString)oper2;
    return NOTHING;
}

GameValue A2LocSetType(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    if (Runtime::Location* loc = LocationOf(oper1))
        loc->type = (const char*)(RString)oper2;
    return NOTHING;
}

GameValue A2LocSetPosition(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    if (Runtime::Location* loc = LocationOf(oper1))
        Position2D(oper2, loc->x, loc->y);
    return NOTHING;
}

GameValue A2LocSetSize(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Runtime::Location* loc = LocationOf(oper1);
    const GameArrayType& a = oper2;
    if (loc && a.Size() >= 2 && a[0].GetType() == GameScalar && a[1].GetType() == GameScalar)
    {
        loc->sizeA = a[0];
        loc->sizeB = a[1];
    }
    return NOTHING;
}

// text location -> its name; text "string" -> the string (structured text is plain here)
GameValue A2Text(const GameState*, GameValuePar oper1)
{
    if (const Runtime::Location* loc = LocationOf(oper1))
        return GameValue(RString(loc->text.c_str()));
    return oper1;
}

GameValue A2LocationPosition(const GameState* state, GameValuePar oper1)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& a = result;
    float x = 0, y = 0;
    if (Position2D(oper1, x, y))
    {
        a.Add(GameValue(x));
        a.Add(GameValue(y));
        a.Add(GameValue(0.0f));
    }
    return result;
}

GameValue A2LocSize(const GameState* state, GameValuePar oper1)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& a = result;
    if (const Runtime::Location* loc = LocationOf(oper1))
    {
        a.Add(GameValue(loc->sizeA));
        a.Add(GameValue(loc->sizeB));
    }
    return result;
}

GameValue A2LocType(const GameState*, GameValuePar oper1)
{
    const Runtime::Location* loc = LocationOf(oper1);
    return GameValue(RString(loc ? loc->type.c_str() : ""));
}

bool TypeMatches(const std::string& type, const GameValue& types)
{
    if (types.GetType() == GameString)
        return _stricmp(type.c_str(), (const char*)(RString)types) == 0;
    if (types.GetType() != GameArray)
        return true;
    const GameArrayType& a = types;
    if (a.Size() == 0)
        return true;
    for (int i = 0; i < a.Size(); i++)
        if (a[i].GetType() == GameString && _stricmp(type.c_str(), (const char*)(RString)a[i]) == 0)
            return true;
    return false;
}

// nearestLocations [position, [types], radius] / nearestLocation [position, type]
std::vector<std::pair<float, int>> NearLocations(GameValuePar oper1, bool limitRadius)
{
    std::vector<std::pair<float, int>> found;
    const GameArrayType& args = oper1;
    float x = 0, y = 0;
    if (args.Size() < 2 || !Position2D(args[0], x, y))
        return found;
    const float radius = limitRadius && args.Size() > 2 && args[2].GetType() == GameScalar ? (float)args[2] : 1e9f;
    Runtime& rt = RT();
    LoadWorldLocations(rt);
    for (size_t i = 0; i < rt.locations.size(); i++)
    {
        const Runtime::Location& loc = rt.locations[i];
        if (loc.deleted || !TypeMatches(loc.type, args[1]))
            continue;
        const float d = std::hypot(loc.x - x, loc.y - y);
        if (d <= radius)
            found.emplace_back(d, static_cast<int>(i));
    }
    std::sort(found.begin(), found.end());
    return found;
}

GameValue A2NearestLocations(const GameState* state, GameValuePar oper1)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& a = result;
    for (const auto& [d, index] : NearLocations(oper1, true))
        a.Add(LocationValue(index));
    return result;
}

GameValue A2NearestLocation(const GameState*, GameValuePar oper1)
{
    const auto found = NearLocations(oper1, false);
    if (found.empty())
        return LocationValue(-1);
    return LocationValue(found.front().second);
}

// ---------------------------------------------------------------------------
// Arma 2 forms of CWA commands
// ---------------------------------------------------------------------------
// createVehicle [type, position, markers, placement, special]
GameValue A2CreateVehicleArray(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& args = oper1;
    if (args.Size() < 2 || args[0].GetType() != GameString)
        return GameValueExt((Object*)nullptr);
    const RString type = args[0];
    if (type.GetLength() > 0 && type[0] == '#')
    {
        // LGT-SCRIPT-001: createVehicle ["#lightpoint", pos, ...] -- the Arma 2/3 array form
        if (stricmp(type, "#lightpoint") == 0)
        {
            Vector3 lp;
            if (GetPos(state, lp, args[1]))
                return GameValueExt(Poseidon::CreateScriptLight(lp));
        }
        return GameValueExt((Object*)nullptr);
    }
    GameValue pos = args[1];
    if (pos.GetType() == GameObject)
        pos = A2GetPosATL(state, pos);
    GameValue created = VehCreate(state, args[0], pos);
    // [type, pos, markers, radius, "FLY"]: an aircraft starts in the air, engine running,
    // at flight speed -- as a mission.sqm vehicle placed "FLY" does (AICenter).
    if (args.Size() >= 5 && args[4].GetType() == GameString && Lower((const char*)(RString)args[4]) == "fly")
    {
        EntityAI* veh = dyn_cast<EntityAI>(GetObject(created));
        if (veh && veh->GetType()->IsKindOf(GWorld->Preloaded(VTypeAir)))
        {
            Vector3 p = veh->Position();
            Matrix4 transform = veh->Transform();
            Matrix3 dir;
            dir.SetUpAndDirection(VUp, veh->Direction());
            transform.SetOrientation(dir);
            veh->SetTransform(transform);
            p[1] = GLandscape->SurfaceYAboveWater(p[0], p[2]) + veh->MakeAirborne();
            transform.SetPosition(p);
            veh->Move(transform);
        }
    }
    return created;
}

// createSoundSource [type, position, markers, placement] (C4_BitterChill's burning houses):
// a CfgVehicles class whose `sound` names a CfgSFX entry, played where it is put -- the
// object a mission.sqm sound source becomes (AICenter's CreateSoundSource), placed the same
// way: one of the markers or the position, then within `placement` metres of it.
GameValue A2CreateSoundSource(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& args = oper1;
    if (args.Size() < 2 || args[0].GetType() != GameString)
        return GameValueExt((Object*)nullptr);
    const RString type = args[0];
    const ParamEntry* cls = (Pars >> "CfgVehicles").FindEntry(type);
    if (!cls || !cls->IsClass())
        cls = Arma1Compat::FindConfigClass("CfgVehicles", type);
    const ParamEntry* soundEntry = cls && cls->IsClass() ? cls->FindEntry("sound") : nullptr;
    const RString sound = soundEntry ? RString(*soundEntry) : RString();
    if (sound.GetLength() == 0)
    {
        LOG_WARN(Script, "createSoundSource: '{}' names no sound", (const char*)type);
        return GameValueExt((Object*)nullptr);
    }
    Vector3 pos(VZero);
    if (!GetPos(state, pos, args[1]))
        return GameValueExt((Object*)nullptr);
    if (args.Size() >= 3 && args[2].GetType() == GameArray)
    {
        const GameArrayType& markers = args[2];
        const int n = markers.Size();
        const int i = n > 0 ? toIntFloor((n + 1) * GRandGen.RandomValue()) : n;
        if (i < n && markers[i].GetType() == GameString)
        {
            Vector3 markerPos;
            if (GetPos(state, markerPos, MarkerGetPos(state, markers[i])))
                pos = markerPos;
        }
    }
    const float placement = args.Size() >= 4 && args[3].GetType() == GameScalar ? (float)args[3] : 0.0f;
    if (placement > 0)
    {
        float xr, zr;
        do
        {
            xr = (2.0f * GRandGen.RandomValue() - 1.0f) * placement;
            zr = (2.0f * GRandGen.RandomValue() - 1.0f) * placement;
        } while (Square(xr) + Square(zr) > placement * placement);
        pos[0] += xr;
        pos[2] += zr;
    }
    pos[1] = GLandscape->SurfaceYAboveWater(pos[0], pos[2]);
    DynSoundSource* source = new DynSoundSource(sound);
    source->SetPosition(pos);
    GWorld->AddBuilding(source);
    LOG_INFO(Script, "createSoundSource '{}': sound '{}' at {:.1f},{:.1f}", (const char*)type, (const char*)sound,
             pos.X(), pos.Z());
    return GameValueExt(source);
}

// unit moveInCargo [vehicle, cargoIndex]
GameValue A2MoveInCargoIndex(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& args = oper2;
    if (args.Size() < 1)
        return NOTHING;
    return ObjMoveInCargo(state, oper1, args[0]);
}

// layer cutText / cutRsc / cutObj [...]: CWA has a single layer
// layer cutText [...]: on its own layer (World::SetCutLayer) -- Arma 3 holds a black canvas on one
// while texts and fades come and go on others (Old Man's animated opening)
GameValue A2CutTextLayer(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const std::string key = Poseidon::Arma3Rsc::LayerKey(oper1);
    Poseidon::Arma3Rsc::CloseLayer(key);
    const int layer = Poseidon::Arma1Compat::Generation() == 3 ? std::atoi(key.c_str()) : 0;
    if (GWorld)
        GWorld->SetCutLayer(layer);
    const GameValue r = ::CutTextEffect(state, oper2);
    if (GWorld)
        GWorld->SetCutLayer(0);
    return r;
}

GameValue A2CutFadeOutLayer(const GameState*, GameValuePar oper1, GameValuePar)
{
    const std::string key = Poseidon::Arma3Rsc::LayerKey(oper1);
    Poseidon::Arma3Rsc::CloseLayer(key);
    if (GWorld && Poseidon::Arma1Compat::Generation() == 3 && std::atoi(key.c_str()) != 0)
    {
        GWorld->SetCutLayer(std::atoi(key.c_str()));
        GWorld->SetCutEffect(nullptr);
        GWorld->SetCutLayer(0);
    }
    return NOTHING;
}

GameValue A2CutRscLayer(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Poseidon::Arma3Rsc::Open(Poseidon::Arma3Rsc::LayerKey(oper1), oper2);
    return ::CutRscEffect(state, oper2);
}

GameValue A2CutObjLayer(const GameState* state, GameValuePar, GameValuePar oper2)
{
    return CutObj(state, oper2);
}
// WFSideText side/unit/group: the side as the Warfare scripts spell it in variable names
GameValue A2WFSideText(const GameState* state, GameValuePar oper1)
{
    const TargetSide side = oper1.GetType() == GameSide ? GetSide(oper1) : GetSide(ObjSide(state, oper1));
    switch (side)
    {
    case TWest:
        return GameValue(RString("West"));
    case TEast:
        return GameValue(RString("East"));
    case TGuerrila:
        return GameValue(RString("Resistance"));
    case TCivilian:
        return GameValue(RString("Civilian"));
    default:
        return GameValue(RString("Unknown"));
    }
}

// obj setVehicleInit "code" ... processInitCommands
GameValue A2SetVehicleInit(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Object* obj = GetObject(oper1);
    if (!obj)
        return NOTHING;
    Runtime& rt = RT();
    rt.vehicleInits.emplace_back(OLink<Object>(obj), std::string((const char*)(RString)oper2));
    return NOTHING;
}

GameValue A2ClearVehicleInit(const GameState*, GameValuePar oper1)
{
    Object* obj = GetObject(oper1);
    auto& list = RT().vehicleInits;
    list.erase(std::remove_if(list.begin(), list.end(), [obj](const auto& e) { return e.first == obj; }), list.end());
    return NOTHING;
}

GameValue A2ProcessInitCommands(const GameState*)
{
    auto pending = std::move(RT().vehicleInits);
    RT().vehicleInits.clear();
    GameState* gstate = GWorld->GetGameState();
    for (auto& [obj, code] : pending)
    {
        if (!obj || code.empty())
            continue;
        gstate->VarSet("this", GameValueExt(obj.GetLink()), true);
        gstate->Execute(code.c_str());
    }
    return NOTHING;
}

// obj attachTo [target, [x, y, z] (model space, z up), memoryPoint]
GameValue A2AttachTo(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Object* obj = GetObject(oper1);
    const GameArrayType& args = oper2;
    Object* target = args.Size() > 0 ? GetObject(args[0]) : nullptr;
    if (!obj || !target || obj == target)
        return NOTHING;
    Vector3 offset(0, 0, 0);
    if (args.Size() > 1 && args[1].GetType() == GameArray)
    {
        const GameArrayType& o = args[1];
        if (o.Size() >= 3 && o[0].GetType() == GameScalar && o[1].GetType() == GameScalar && o[2].GetType() == GameScalar)
            offset = Vector3(static_cast<float>(o[0]), static_cast<float>(o[2]), static_cast<float>(o[1]));
    }
    else
        offset = target->GetInvTransform() * obj->Position();
    auto& list = RT().attachments;
    list.erase(std::remove_if(list.begin(), list.end(), [obj](const auto& a) { return a.object == obj; }), list.end());
    list.push_back({OLink<Object>(obj), OLink<Object>(target), offset});
    return NOTHING;
}

GameValue A2Detach(const GameState*, GameValuePar oper1)
{
    Object* obj = GetObject(oper1);
    auto& list = RT().attachments;
    list.erase(std::remove_if(list.begin(), list.end(), [obj](const auto& a) { return a.object == obj; }), list.end());
    return NOTHING;
}

void UpdateAttachments(Runtime& rt)
{
    GameState* gstate = GWorld->GetGameState();
    for (size_t i = 0; i < rt.attachments.size();)
    {
        Runtime::Attachment& a = rt.attachments[i];
        if (!a.object || !a.target)
        {
            rt.attachments.erase(rt.attachments.begin() + i);
            continue;
        }
        const Vector3 p = a.target->PositionModelToWorld(a.offset);
        GameValue pos = gstate->CreateGameValue(GameArray);
        GameArrayType& arr = pos;
        arr.Add(GameValue(p.X()));
        arr.Add(GameValue(p.Z()));
        arr.Add(GameValue(p.Y() - GLandscape->SurfaceY(p.X(), p.Z())));
        ObjSetPos(gstate, GameValueExt(a.object.GetLink()), pos);
        i++;
    }
}
// ---------------------------------------------------------------------------
// Triggers, waypoints, vehicles
// ---------------------------------------------------------------------------
GameValue A2TriggerActivated(const GameState*, GameValuePar oper1)
{
    Detector* det = dyn_cast<Detector>(GetObject(oper1));
    return GameValue(det && det->IsActive());
}

GameValue A2TriggerArea(const GameState* state, GameValuePar oper1)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& a = result;
    if (Detector* det = dyn_cast<Detector>(GetObject(oper1)))
    {
        a.Add(GameValue(det->GetAreaA()));
        a.Add(GameValue(det->GetAreaB()));
        a.Add(GameValue(det->GetAreaAngleDeg()));
        a.Add(GameValue(det->IsAreaRectangular()));
    }
    return result;
}

AIGroup* GroupOf(GameValuePar v)
{
    if (v.GetType() == GameGroup)
        return GetGroup(v);
    if (EntityAI* e = dyn_cast<EntityAI>(GetObject(v)))
        if (AIUnit* u = e->CommanderUnit())
            return u->GetGroup();
    return nullptr;
}

// expectedDestination unit: [position, planningMode, forceReplan] -- where the unit's AI
// is heading (CE7B's HALO script steers each parachute towards it). Without a plan the
// unit's own position and "DoNotPlan", as Arma 2 answers for a unit that is not moving.
GameValue A2ExpectedDestination(const GameState* state, GameValuePar oper1)
{
    Object* obj = GetObject(oper1);
    Vector3 pos = obj ? obj->Position() : VZero;
    const char* mode = "DoNotPlan";
    if (EntityAI* e = dyn_cast<EntityAI>(obj))
        if (AIUnit* u = e->CommanderUnit())
        {
            const Vector3 wanted = u->GetWantedPosition();
            if (wanted.SquareSize() > 0.0f && wanted.Distance2(pos) > 1.0f)
            {
                pos = wanted;
                mode = "LEADER PLANNED";
            }
        }
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& array = result;
    GameValue p = state->CreateGameValue(GameArray);
    GameArrayType& pa = p;
    pa.Add(GameValue(pos.X()));
    pa.Add(GameValue(pos.Z()));
    pa.Add(GameValue(pos.Y() - GLandscape->SurfaceY(pos.X(), pos.Z())));
    array.Add(p);
    array.Add(GameValue(RString(mode)));
    array.Add(GameValue(false));
    return result;
}

GameValue A2CurrentWaypoint(const GameState*, GameValuePar oper1)
{
    AIGroup* grp = GroupOf(oper1);
    if (!grp)
        return GameValue(0.0f);
    auto* cur = grp->GetCurrent();
    if (!cur || !cur->_fsm)
        return GameValue(static_cast<float>(grp->NWaypoints()));
    return GameValue(static_cast<float>(cur->_fsm->Var(0)));
}

// group setCurrentWaypoint [group, index]
GameValue A2SetCurrentWaypoint(const GameState*, GameValuePar, GameValuePar oper2)
{
    const GameArrayType& wp = oper2;
    if (wp.Size() < 2 || wp[1].GetType() != GameScalar)
        return NOTHING;
    AIGroup* grp = GroupOf(wp[0]);
    if (!grp)
        return NOTHING;
    auto* cur = grp->GetCurrent();
    const int index = static_cast<int>(static_cast<float>(wp[1]));
    if (cur && cur->_fsm && index >= 0 && index <= grp->NWaypoints())
        cur->_fsm->Var(0) = index;
    return NOTHING;
}

// vehicle emptyPositions "driver" | "gunner" | "commander" | "cargo"
GameValue A2EmptyPositions(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Transport* t = dyn_cast<Transport>(GetObject(oper1));
    if (!t)
        return GameValue(0.0f);
    const RString what = oper2;
    int n = 0;
    if (_stricmp(what, "driver") == 0)
        n = t->Type()->HasDriver() && !t->Driver() ? 1 : 0;
    else if (_stricmp(what, "gunner") == 0)
        n = t->Type()->HasGunner() && !t->Gunner() ? 1 : 0;
    else if (_stricmp(what, "commander") == 0)
        n = t->Type()->HasCommander() && !t->Commander() ? 1 : 0;
    else if (_stricmp(what, "cargo") == 0)
        n = std::max(0, t->GetMaxManCargo() - t->GetManCargoSize());
    return GameValue(static_cast<float>(n));
}

GameValue A2SideUnknown(const GameState*)
{
    return CreateGameSide(TSideUnknown);
}

// mapGridPosition object|position: six-digit grid reference (100 m squares)
GameValue A2MapGridPosition(const GameState*, GameValuePar oper1)
{
    float x = 0, y = 0;
    Position2D(oper1, x, y);
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%03d%03d", std::clamp(static_cast<int>(x / 100), 0, 999),
             std::clamp(static_cast<int>(y / 100), 0, 999));
    return GameValue(RString(buffer));
}

GameValue A2MinusOneUnary(const GameState*, GameValuePar)
{
    return GameValue(-1.0f);
}

GameValue A2ZeroBinary(const GameState*, GameValuePar, GameValuePar)
{
    return GameValue(0.0f);
}

GameValue A2EmptyString(const GameState*)
{
    return GameValue(RString(""));
}

GameValue A2ZeroPos(const GameState* state, GameValuePar, GameValuePar)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& a = result;
    for (int i = 0; i < 3; i++)
        a.Add(GameValue(0.0f));
    return result;
}

// object selectionPosition "name" (or [name, lod]): the named selection's centre in model
// space, [x, y, z] with z up, as modelToWorld takes it -- from the memory LOD, else the
// first LOD. Was always [0, 0, 0]; Arma 3's carrier puts each hull part at its base's
// memory point this way, so the whole ship was stacked on the base at sea level.
GameValue A2SelectionPosition(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& a = result;
    Vector3 centre = VZero;
    Object* obj = GetObject(oper1);
    RString name;
    if (oper2.GetType() == GameString)
        name = oper2;
    else if (oper2.GetType() == GameArray && static_cast<const GameArrayType&>(oper2).Size() > 0 &&
             static_cast<const GameArrayType&>(oper2)[0].GetType() == GameString)
        name = static_cast<const GameArrayType&>(oper2)[0];
    LODShape* shape = obj ? obj->GetShape() : nullptr;
    if (shape && name.GetLength() > 0)
    {
        Shape* levels[2] = {shape->MemoryLevel(), shape->NLevels() > 0 ? shape->Level(0) : nullptr};
        for (Shape* level : levels)
        {
            const int sel = level ? level->FindNamedSel(name) : -1;
            if (sel < 0)
                continue;
            const NamedSelection& points = level->NamedSel(sel);
            if (points.Size() <= 0)
                continue;
            for (int i = 0; i < points.Size(); i++)
                centre += level->Pos(points[i]);
            centre /= static_cast<float>(points.Size());
            break;
        }
    }
    static int logged = 0;
    if (obj && logged < 40)
    {
        logged++;
        const Vector3 world = obj->PositionModelToWorld(centre);
        LOG_INFO(Mission, "selectionPosition '{}' on {}: model {:.2f},{:.2f},{:.2f} world {:.1f},{:.1f},{:.1f} (object at {:.1f},{:.1f},{:.1f})",
                 (const char*)name, shape ? (const char*)shape->Name() : "?", centre.X(), centre.Y(), centre.Z(),
                 world.X(), world.Y(), world.Z(), obj->Position().X(), obj->Position().Y(), obj->Position().Z());
    }
    a.Add(GameValue(centre.X()));
    a.Add(GameValue(centre.Z()));
    a.Add(GameValue(centre.Y()));
    return result;
}
} // namespace

// Arma 2 briefing: the diary and the simple tasks, as the HTML sections the CWA map
// notebook shows ("Main" = diary, "Plan" = tasks), added when the mission's own
// briefing.html has none; <localized id="..."/> tags are resolved.
namespace
{
std::string SanitizeRichText(const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); i++)
    {
        if (in[i] != '<')
        {
            out += in[i];
            continue;
        }
        const size_t end = in.find('>', i);
        if (end == std::string::npos)
            break;
        std::string tag = Lower(in.substr(i + 1, end - i - 1));
        while (!tag.empty() && (tag.back() == '/' || tag.back() == ' '))
            tag.pop_back();
        const std::string name = tag.substr(0, tag.find(' '));
        if (name == "br")
            out += "<br>";
        else if (name == "b" || name == "/b" || name == "i" || name == "/i" || name == "p" || name == "/p")
            out += "<" + name + ">";
        i = end;
    }
    return out;
}

std::string ResolveLocalizedTags(const std::string& html)
{
    std::string out;
    size_t pos = 0;
    while (true)
    {
        const size_t at = html.find("<localized", pos);
        if (at == std::string::npos)
        {
            out.append(html, pos, std::string::npos);
            return out;
        }
        out.append(html, pos, at - pos);
        const size_t end = html.find('>', at);
        if (end == std::string::npos)
        {
            out.append(html, at, std::string::npos);
            return out;
        }
        const std::string tag = html.substr(at, end - at);
        const size_t id = tag.find("id=");
        std::string key;
        if (id != std::string::npos && id + 4 <= tag.size())
        {
            const char q = tag[id + 3];
            const size_t close = tag.find(q, id + 4);
            if (close != std::string::npos)
                key = tag.substr(id + 4, close - id - 4);
        }
        if (!key.empty())
            out += (const char*)LocalizeString(key.c_str());
        pos = end + 1;
    }
}
} // namespace

int Arma2BriefingRevision()
{
    return Arma1Compat::Generation() >= 2 ? RT().revision : 0;
}

// The diary as Arma 3's map lists it: {subject, title, text} per record, in the order they were made,
// then one per task under the subject "Tasks" (its title carrying its state). Text is the script's own
// structured text.
std::vector<std::array<std::string, 3>> Arma2DiaryRecords()
{
    std::vector<std::array<std::string, 3>> out;
    const Runtime& rt = RT();
    for (const Runtime::DiaryEntry& e : rt.diary)
        out.push_back({e.subject, e.title, e.text});
    std::vector<std::string> shown;
    for (const Task& t : rt.tasks)
    {
        if (std::find(shown.begin(), shown.end(), t.name) != shown.end())
            continue; // one entry per task, not per group member
        shown.push_back(t.name);
        const char* state = t.state == "SUCCEEDED" ? " (accomplished)" : t.state == "FAILED" ? " (failed)"
                            : t.state == "CANCELED" ? " (canceled)" : "";
        out.push_back({"Tasks", t.name + state, t.description});
    }
    return out;
}

std::string Arma2BriefingHtml(const std::string& html)
{
    if (Arma1Compat::Generation() < 2)
        return html;
    std::string out = ResolveLocalizedTags(html);
    const std::string lower = Lower(out);
    const Runtime& rt = RT();
    if (lower.find("name=\"main\"") == std::string::npos && !rt.diary.empty())
    {
        out += "\n<hr>\n<p><a name=\"Main\"></a></p>\n";
        for (auto it = rt.diary.rbegin(); it != rt.diary.rend(); ++it) // newest first, as Arma 2 lists them
            out += "<h2>" + SanitizeRichText(it->title) + "</h2>\n<p>" + SanitizeRichText(it->text) + "</p>\n";
    }
    if (lower.find("name=\"plan\"") == std::string::npos && !rt.tasks.empty())
    {
        out += "\n<hr>\n<p><a name=\"Plan\"></a></p>\n";
        std::vector<std::string> shown;
        for (const Task& t : rt.tasks)
        {
            if (std::find(shown.begin(), shown.end(), t.name) != shown.end())
                continue; // one entry per task, not per group member
            shown.push_back(t.name);
            const char* state = t.state == "SUCCEEDED" ? " (accomplished)" : t.state == "FAILED" ? " (failed)"
                                : t.state == "CANCELED" ? " (canceled)" : "";
            out += "<h2>" + SanitizeRichText(t.name) + state + "</h2>\n<p>" + SanitizeRichText(t.description) + "</p>\n";
        }
    }
    out += "\n<hr>\n";
    return out;
}

// Runs the scripted FSMs one transition per simulation step (World::SimulateScripts).

// waypoints group|unit: [[group, 0], [group, 1], ...] (index 0 is the start position)
GameValue A2Waypoints(const GameState* state, GameValuePar oper1)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& out = result;
    AIGroup* grp = GroupOf(oper1);
    for (int i = 0; grp && i < grp->NWaypoints(); i++)
    {
        GameArrayType wp;
        wp.Add(GameValueExt(grp));
        wp.Add(GameValue(static_cast<float>(i)));
        out.Add(GameValue(wp));
    }
    return result;
}

// ---------------------------------------------------------------------------
// synchronizedObjects: the mission.sqm "synchronizations[]" links between objects
// (a Warfare module and the teams it runs, say). Links go both ways.
// ---------------------------------------------------------------------------
GameValue A2SynchronizedObjects(const GameState* state, GameValuePar oper1)
{
    GameValue value = state->CreateGameValue(GameArray);
    GameArrayType& out = value;
    Object* obj = GetObject(oper1);
    Runtime& rt = RT();
    int self = -1;
    for (const auto& [id, o] : rt.missionObjects)
        if (o.GetLink() == obj)
            self = id;
    if (!obj || self < 0)
        return value;
    std::vector<int> ids;
    for (const auto& [id, sync] : rt.missionSync)
    {
        if (id == self)
            ids.insert(ids.end(), sync.begin(), sync.end());
        else if (std::find(sync.begin(), sync.end(), self) != sync.end())
            ids.push_back(id);
    }
    std::vector<Object*> seen;
    for (int id : ids)
    {
        const auto it = rt.missionObjects.find(id);
        Object* o = it != rt.missionObjects.end() ? it->second.GetLink() : nullptr;
        if (!o || o == obj || std::find(seen.begin(), seen.end(), o) != seen.end())
            continue;
        seen.push_back(o);
        out.Add(GameValueExt(o));
    }
    return value;
}

// Arma 2 addAction: arguments are passed as _this select 3; the script is a .sqf (run
// like execVM), a .sqs, or code.
void Arma2SetUserActionArguments(EntityAI* veh, int id, GameValuePar arguments)
{
    RT().actionArguments[{veh, id}] = arguments;
}

// Called for every object mission.sqm creates (AICenterImpl.cpp).
void Arma2RegisterMissionObject(int id, Object* obj, const std::vector<int>& sync)
{
    if (id < 0 || !obj)
        return;
    Runtime& rt = RT();
    rt.missionObjects[id] = obj;
    if (!sync.empty())
        rt.missionSync.emplace_back(id, sync);
}

GameValue ScriptExecVM(const GameState* state, GameValuePar oper1, GameValuePar oper2); // Scripts.cpp

bool Arma2RunUserAction(EntityAI* veh, Person* caller, int id, const RString& script)
{
    if (Arma1Compat::Generation() < 2)
        return false;
    GameState* gstate = GWorld->GetGameState();
    GameValue args = gstate->CreateGameValue(GameArray);
    GameArrayType& a = args;
    a.Add(GameValueExt(veh));
    a.Add(GameValueExt(caller));
    a.Add(GameValue(static_cast<float>(id)));
    auto& map = RT().actionArguments;
    const auto found = map.find({veh, id});
    a.Add(found != map.end() ? found->second : GameValue());
    const std::string name = Lower((const char*)script);
    auto endsWith = [&](const char* ext) {
        const size_t n = strlen(ext);
        return name.size() >= n && name.compare(name.size() - n, n, ext) == 0;
    };
    if (endsWith(".sqs"))
        return false; // CWA's own path runs .sqs files
    if (endsWith(".sqf"))
        ScriptExecVM(gstate, args, GameValue(script));
    else
        GWorld->AddScript(Script::CreateFromCode("addAction", script, args));
    return true;
}

// ---------------------------------------------------------------------------
// Stall report (--arma2-test-seconds only). A mission that freezes the main loop leaves
// nothing in the log; this names the expression the evaluator last started, every 10 s
// while the simulation does not advance.
// ---------------------------------------------------------------------------
namespace
{
// a copy, not the evaluator's pointer: the expression's text may be freed by the time the
// monitor reads it (the WGPU runs logged garbage there)
std::mutex GLastExpressionLock;
char GLastExpression[256] = {};
std::atomic<unsigned> GSimulateTick{0};

void StallMonitor()
{
    unsigned last = GSimulateTick.load();
    int stalled = 0;
    // Until the mission's first simulation step the game is still loading: mounting the
    // archives, the world, and under WGPU the renderer's pipelines (compiled on first use --
    // on the laptop's Intel GPU under DX12 that alone ran past the 60 s). Up to then it gets
    // 180 s, afterwards 60 s without a step end a test run.
    bool simulating = false;
    // Arma 3 mounts some 550 archives and its steps can be long (preInit compiles): 120 s per step.
    const bool arma3 = Arma1Compat::Generation() >= 3;
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const unsigned now = GSimulateTick.load();
        if (now != last || (arma3 && now == 0))
        {
            last = now;
            stalled = 0;
            simulating = true;
            continue;
        }
        if (++stalled % 10 != 0)
            continue;
        std::string expr;
        {
            std::lock_guard<std::mutex> lock(GLastExpressionLock);
            for (int i = 0; i < (int)sizeof(GLastExpression) && GLastExpression[i]; i++)
                expr.push_back(GLastExpression[i] == '\n' || GLastExpression[i] == '\r' ? ' ' : GLastExpression[i]);
        }
        LOG_WARN(Script, "{} stall: no simulation step for {} s; evaluator last started: {}", Arma1Compat::Label(),
                 stalled, expr);
        if (stalled == 10 || stalled == 30 || stalled == 90 || stalled == 170)
            Poseidon::Foundation::LogStallThreadStack();
        if (stalled >= (simulating ? (arma3 ? 120 : 60) : 180))
        {
            // a test run that cannot continue ends instead of holding the batch
            LOG_ERROR(Script, "{} stall: giving up after {} s (test mode)", Arma1Compat::Label(), stalled);
            std::_Exit(3);
        }
    }
}

void TraceExpression(const char* expression)
{
    std::lock_guard<std::mutex> lock(GLastExpressionLock);
    if (!expression)
    {
        GLastExpression[0] = 0;
        return;
    }
    std::strncpy(GLastExpression, expression, sizeof(GLastExpression) - 1);
    GLastExpression[sizeof(GLastExpression) - 1] = 0;
}
} // namespace

void Arma2StartStallMonitor()
{
    static bool started = false;
    if (started || Arma1Compat::Generation() < 2 ||
        Poseidon::Foundation::AppConfig::Instance().GetArma1TestSeconds() <= 0.0f)
        return;
    started = true;
    Poseidon::Foundation::RememberStallThread();
    GameState::s_traceExpression = TraceExpression;
    std::thread(StallMonitor).detach();
}

void Arma3RuntimeSimulate(); // GameStateExtA3.cpp

void Arma2RuntimeSimulate()
{
    GSimulateTick.fetch_add(1, std::memory_order_relaxed);
    Arma3RuntimeSimulate();
    if (Arma1Compat::Generation() < 2 || !GWorld || !GWorld->GetGameState())
        return;
    Runtime& rt = RT();
    KbStep(rt);
    for (size_t i = 0; i < rt.fsms.size(); i++) // FSMs may start FSMs
        FsmStep(*rt.fsms[i]);
    UpdateAttachments(rt);

    // Diagnostics: OP_A2_WATCH="var1,var2,..." logs those variables every OP_A2_PERIOD s of game time.
    static const std::string watch = [] {
        const char* e = std::getenv("OP_A2_WATCH");
        return std::string(e ? e : "");
    }();
    // OP_A2_EVAL="expression" is evaluated and logged at the same times; OP_A2_PERIOD sets
    // the interval in seconds (default 10).
    static const std::string eval = [] {
        const char* e = std::getenv("OP_A2_EVAL");
        return std::string(e ? e : "");
    }();
    static const float period = [] {
        const char* e = std::getenv("OP_A2_PERIOD");
        const float p = e ? static_cast<float>(atof(e)) : 10.0f;
        return p > 0.05f ? p : 10.0f;
    }();
    static float nextWatch = 0;
    if ((!watch.empty() || !eval.empty()) && (Now() >= nextWatch || Now() + 1 < nextWatch - period))
    {
        nextWatch = Now() + period;
        GameState* gstate = GWorld->GetGameState();
        size_t pos = 0;
        while (pos < watch.size())
        {
            size_t end = watch.find(',', pos);
            if (end == std::string::npos)
                end = watch.size();
            const std::string name = watch.substr(pos, end - pos);
            if (!name.empty())
            {
                const GameValue v = gstate->VarGet(name.c_str());
                LOG_INFO(Script, "watch t={:.0f} {} = {}", Now(), name, v.GetNil() ? "<nil>" : (const char*)v.GetText());
            }
            pos = end + 1;
        }
        if (!eval.empty())
        {
            const GameValue v = gstate->EvaluateMultiple(eval.c_str());
            LOG_INFO(Script, "eval t={:.0f} {} = {}", Now(), eval, v.GetNil() ? "<nil>" : (const char*)v.GetText());
        }
    }
}

namespace
{
const GameNular A2Nular[] = {
    GameNular(GameString, "missionNamespace", A2MissionNamespace),
    GameNular(GameString, "uiNamespace", A2UiNamespace),
    GameNular(GameString, "parsingNamespace", A2MissionNamespace),
    GameNular(GameString, "profileNamespace", A2UiNamespace),
    GameNular(GameArray, "allUnits", A2AllUnits),
    GameNular(GameArray, "allGroups", A2AllGroups),
    GameNular(GameScalar, "diag_tickTime", A2DiagTickTime),
    GameNular(GameScalar, "diag_frameNo", A2DiagFrameNo),
    GameNular(GameScalar, "diag_fps", A2DiagFps),
    GameNular(GameBool, "isMultiplayer", A2False),
    GameNular(GameBool, "isDedicated", A2False),
    GameNular(GameScalar, "serverTime", GameTime),
    GameNular(GameSide, "playerSide", A2PlayerSide),
    GameNular(GameScalar, "safeZoneX", A2SafeZoneXNular),
    GameNular(GameScalar, "safeZoneY", A2SafeZoneYNular),
    GameNular(GameScalar, "safeZoneW", A2SafeZoneWNular),
    GameNular(GameScalar, "safeZoneH", A2SafeZoneHNular),
    GameNular(GameScalar, "safeZoneXAbs", A2SafeZoneXNular),
    GameNular(GameScalar, "safeZoneWAbs", A2SafeZoneWNular),
    GameNular(GameString, "locationNull", A2LocationNull),
    GameNular(GameNothing, "disableSerialization", A2NothingNular),
    GameNular(GameNothing, "endLoadingScreen", A2NothingNular),
    GameNular(GameNothing, "clearRadio", A2NothingNular),
    GameNular(GameBool, "visibleMap", A2False),
    GameNular(GameBool, "savingEnabled", A2True),
    GameNular(GameBool, "teamSwitchEnabled", A2False),
    GameNular(GameScalar, "viewDistance", A2One),
    GameNular(GameNothing, "processInitCommands", A2ProcessInitCommands),
    GameNular(GameString, "campaignConfigFile", A1MissionConfigFile),
    GameNular(GameSide, "sideUnknown", A2SideUnknown),
    GameNular(GameArray, "playableUnits", A2EmptyArrayNular),
    GameNular(GameArray, "switchableUnits", A2SwitchableUnits),
    GameNular(GameString, "worldName", A2WorldName),
    GameNular(GameObject, "cursorTarget", A2ObjNull),
    GameNular(GameScalar, "overcast", A2Overcast),
    GameNular(GameScalar, "fog", A2Fog),
    GameNular(GameScalar, "rain", A2Rain),
    GameNular(GameArray, "wind", A2Wind),
    GameNular(GameScalar, "estimatedEndServerTime", A2Zero),
    GameNular(GameBool, "serverCommandAvailable", A2False),
    GameNular(GameArray, "groupIconsVisible", A2EmptyArrayNular),
    GameNular(GameString, "commandingMenu", A2EmptyString),
    GameNular(GameScalar, "displayNull", A2MinusOne),
    GameNular(GameScalar, "controlNull", A2MinusOne),
    GameNular(GameBool, "hcShownBar", A2False),
    GameNular(GameArray, "hcSelected", A2EmptyArrayNular),
};

const GameFunction A2Unary[] = {
    GameFunction(GameNothing, "textLogFormat", A2TextLogFormat, GameArray),
    GameFunction(GameNothing, "onPreloadStarted", A2NothingUnary, GameString),
    GameFunction(GameNothing, "onPreloadFinished", A2NothingUnary, GameString),
    GameFunction(GameNothing, "scriptName", A2NothingUnary, GameString),
    GameFunction(GameNothing, "startLoadingScreen", A2NothingUnary, GameArray),
    GameFunction(GameNothing, "progressLoadingScreen", A2NothingUnary, GameScalar),
    GameFunction(GameNothing, "activateAddons", A2NothingUnary, GameArray),
    GameFunction(GameNothing, "enableSaving", A2NothingUnary, GameArray | GameBool),
    GameFunction(GameNothing, "enableSentences", A2NothingUnary, GameBool),
    GameFunction(GameNothing, "showCommandingMenu", A2NothingUnary, GameString),
    GameFunction(GameNothing, "setWind", A2NothingUnary, GameArray),
    GameFunction(GameNothing, "camUseNVG", A2NothingUnary, GameBool),
    GameFunction(GameNothing, "hintSilent", A2HintSilent, GameString),
    GameFunction(GameNothing, "endMission", A2EndMission, GameString),
    GameFunction(GameNothing, "failMission", A2FailMission, GameString),
    GameFunction(GameNothing, "taskHint", A2TaskHint, GameArray),
    GameFunction(GameNothing, "ppEffectDestroy", A2NothingUnary, GameScalar | GameArray),
    GameFunction(GameScalar | GameArray, "ppEffectCreate", A2PpEffectCreate, GameArray),
    GameFunction(GameBool, "ppEffectCommitted", A2TrueUnary, GameScalar | GameString),
    GameFunction(GameString, "createMarkerLocal", MarkerCreate, GameArray),
    GameFunction(GameNothing, "deleteMarkerLocal", MarkerDelete, GameString),
    GameFunction(GameScalar, "markerDir", MarkerGetDir, GameString),
    GameFunction(GameScalar, "markerAlpha", A2OneUnary, GameString),
    GameFunction(GameString, "taskState", A2TaskState, GameString),
    GameFunction(GameBool, "taskCompleted", A2TaskCompleted, GameString),
    GameFunction(GameArray, "taskDescription", A2TaskDescription, GameString),
    GameFunction(GameArray, "taskDestination", A2TaskDestination, GameString),
    GameFunction(GameString, "currentTask", A2CurrentTask, GameObject),
    GameFunction(GameArray, "simpleTasks", A2SimpleTasks, GameObject),
    GameFunction(GameScalar, "execFSM", A2ExecFSMUnary, GameString),
    GameFunction(GameBool, "moveToCompleted", A2MoveToCompleted, GameObject),
    GameFunction(GameBool, "moveToFailed", A2MoveToFailed, GameObject),
    GameFunction(GameBool, "completedFSM", A2CompletedFSM, GameScalar),
    GameFunction(GameString, "lifeState", A2LifeState, GameObject),
    GameFunction(GameString, "rank", A2Rank, GameObject),
    GameFunction(GameString, "vehicleVarName", A2VehicleVarName, GameObject),
    GameFunction(GameArray, "nearestObjects", A2NearestObjects, GameArray),
    GameFunction(GameArray, "getPosATL", A2GetPosATL, GameObject),
    GameFunction(GameString, "currentWeapon", ObjGetPrimaryWeapon, GameObject),
    GameFunction(GameObject, "effectiveCommander", ObjCommander, GameObject),
    GameFunction(GameString, "faction", A2Faction, GameObject),
    GameFunction(GameArray, "synchronizedObjects", A2SynchronizedObjects, GameObject),
    GameFunction(GameString, "actionKeysNames", A2ActionKeysNames, GameString | GameArray),
    GameFunction(GameArray, "actionKeysNamesArray", A2ActionKeysNamesArray, GameString | GameArray),
    GameFunction(GameNothing, "hcRemoveAllGroups", A2NothingUnary, GameObject),
    GameFunction(GameString, "createLocation", A2CreateLocation, GameArray),
    GameFunction(GameNothing, "deleteLocation", A2DeleteLocation, GameString),
    GameFunction(GameString, "text", A2Text, GameString),
    GameFunction(GameArray, "locationPosition", A2LocationPosition, GameString),
    GameFunction(GameArray, "position", A2LocationPosition, GameString),
    GameFunction(GameArray, "size", A2LocSize, GameString),
    GameFunction(GameString, "type", A2LocType, GameString),
    GameFunction(GameArray, "nearestLocations", A2NearestLocations, GameArray),
    GameFunction(GameString, "nearestLocation", A2NearestLocation, GameArray),
    GameFunction(GameObject, "createVehicle", A2CreateVehicleArray, GameArray),
    GameFunction(GameObject, "createVehicleLocal", A2CreateVehicleArray, GameArray),
    GameFunction(GameObject, "createSoundSource", A2CreateSoundSource, GameArray),
    GameFunction(GameArray, "expectedDestination", A2ExpectedDestination, GameObject),
    GameFunction(GameNothing, "cutFadeOut", A2NothingUnary, GameScalar),
    GameFunction(GameObject, "hcLeader", A2ObjNullUnary, GameGroup),
    GameFunction(GameString, "WFSideText", A2WFSideText, GameSide | GameObject | GameGroup),
    GameFunction(GameNothing, "clearVehicleInit", A2ClearVehicleInit, GameObject),
    GameFunction(GameNothing, "detach", A2Detach, GameObject),
    GameFunction(GameNothing, "deleteCollection", A2DeleteCollection, GameObject),
    GameFunction(GameObject, "createAgent", A2CreateAgent, GameArray),
    GameFunction(GameArray, "toArray", A2ToArray, GameString),
    GameFunction(GameString, "with", A2With, GameString),
    GameFunction(GameNothing, "hideObject", A2NothingUnary, GameObject),
    GameFunction(GameScalar, "inputAction", A2ZeroUnary, GameString),
    GameFunction(GameNothing, "showHUD", A2ShowHud, GameBool),
    GameFunction(GameNothing, "showHUD", A2ShowHud, GameArray),
    GameFunction(GameNothing, "moveOut", A2MoveOut, GameObject),
    GameFunction(GameNothing, "playSound", A2PlaySoundArray, GameArray),
    GameFunction(GameObject, "unitBackpack", A2ObjNullUnary, GameObject),
    GameFunction(GameNothing, "removeBackpack", A2RemoveBackpack, GameObject),
    GameFunction(GameNothing, "ctrlMapAnimCommit", A2NothingUnary, GameAny),
    GameFunction(GameString, "keyName", A2TextLogEmpty, GameScalar),
    GameFunction(GameScalar, "currentVisionMode", A2ZeroUnary, GameObject),
    GameFunction(GameString, "toString", A2ToString, GameArray),
    GameFunction(GameString, "toLower", A2ToLower, GameString),
    GameFunction(GameString, "toUpper", A2ToUpper, GameString),
    GameFunction(GameScalar, "parseNumber", A2ParseNumber, GameString),
    GameFunction(GameScalar, "needReload", A2ZeroUnary, GameObject),
    GameFunction(GameNothing, "setGroupIconsVisible", A2NothingUnary, GameArray),
    GameFunction(GameString, "actionKeysImages", A2TextLogEmpty, GameString | GameArray),
    GameFunction(GameBool, "triggerActivated", A2TriggerActivated, GameObject),
    GameFunction(GameBool, "isNull", A2IsNullHandle, GameScalar),
    GameFunction(GameBool, "ctrlShown", A2CtrlShown, GameScalar),
    GameFunction(GameBool, "isNull", A2IsNullLocation, GameString),
    GameFunction(GameArray, "items", A2EmptyArrayUnary, GameObject),
    GameFunction(GameArray, "assignedItems", A2EmptyArrayUnary, GameObject),
    GameFunction(GameArray, "triggerArea", A2TriggerArea, GameObject),
    GameFunction(GameScalar, "currentWaypoint", A2CurrentWaypoint, GameGroup | GameObject),
    GameFunction(GameString, "mapGridPosition", A2MapGridPosition, GameObject | GameArray),
    GameFunction(GameScalar, "moveTime", A2ZeroUnary, GameObject),
    GameFunction(GameNothing, "openMap", A2NothingUnary, GameBool | GameArray),
    GameFunction(GameNothing, "addSwitchableUnit", A2NothingUnary, GameObject),
    GameFunction(GameNothing, "removeSwitchableUnit", A2NothingUnary, GameObject),
    GameFunction(GameArray, "hcAllGroups", A2EmptyArrayUnary, GameObject),
    GameFunction(GameScalar, "lnbCurSelRow", A2MinusOneUnary, GameScalar | GameArray),
    GameFunction(GameNothing, "lnbClear", A2NothingUnary, GameScalar | GameArray),
    GameFunction(GameScalar, "lnbAddRow", A2ZeroUnary, GameArray),
    GameFunction(GameScalar, "lnbValue", A2ZeroUnary, GameArray),
    GameFunction(GameString, "lnbData", A2TextLogEmpty, GameArray),
    GameFunction(GameString, "lnbText", A2TextLogEmpty, GameArray),
    GameFunction(GameNothing, "lnbSetValue", A2NothingUnary, GameArray),
    GameFunction(GameNothing, "lnbSetData", A2NothingUnary, GameArray),
    GameFunction(GameNothing, "lnbSetCurSelRow", A2NothingUnary, GameArray),
    GameFunction(GameNothing, "lnbSetPicture", A2NothingUnary, GameArray),
    GameFunction(GameArray, "actionKeys", A2EmptyArrayUnary, GameString),
    GameFunction(GameNothing, "ctrlActivate", A2NothingUnary, GameScalar),
    GameFunction(GameBool, "serverCommand", A2ServerCommand, GameString),
    GameFunction(GameNothing, "closeDisplay", A2NothingUnary, GameScalar | GameArray),
    GameFunction(GameNothing, "copyToClipboard", A2NothingUnary, GameString),
    GameFunction(GameArray, "nearRoads", A2EmptyArrayUnary, GameArray),
    GameFunction(GameArray, "roadsConnectedTo", A2EmptyArrayUnary, GameObject),
    GameFunction(GameBool, "isOnRoad", A2ServerCommand, GameObject | GameArray),
    GameFunction(GameString, "getPlayerUID", A2TextLogEmpty, GameObject),
    GameFunction(GameArray, "assignedCargo", A2EmptyArrayUnary, GameObject),
    GameFunction(GameBool, "difficultyEnabled", A2ServerCommand, GameString),
    GameFunction(GameScalar, "sizeOf", A2ZeroUnary, GameString),
    GameFunction(GameNothing, "lbSort", A2NothingUnary, GameScalar | GameArray),
    GameFunction(GameNothing, "lnbSetText", A2NothingUnary, GameArray),
    GameFunction(GameNothing, "lnbSetColor", A2NothingUnary, GameArray),
    GameFunction(GameArray, "getGroupIcon", A2EmptyArrayUnary, GameArray),
    GameFunction(GameScalar, "ctrlFade", A2CtrlFade, GameScalar),
    GameFunction(GameNothing, "setCamUseTi", A2NothingUnary, GameArray),
    GameFunction(GameScalar, "ctrlMapScale", A2OneUnary, GameScalar),
    GameFunction(GameString, "createDiaryLink", A2TextLogEmpty, GameArray),
    GameFunction(GameString, "waypointDescription", A2TextLogEmpty, GameArray),
    GameFunction(GameArray, "getGroupIconParams", A2GroupIconParams, GameGroup),
    GameFunction(GameScalar, "lnbSize", A2ZeroUnary, GameScalar | GameArray),
};

const GameOperator A2Binary[] = {
    GameOperator(GameScalar, "factor", function, A2Factor, GameScalar, GameArray),
    GameOperator(GameScalar, "getFriend", function, A2GetFriend, GameSide, GameSide),
    GameOperator(GameNothing, "setVariable", function, A2SetVariable, GameObject | GameGroup | GameString, GameArray),
    GameOperator(GameAny, "getVariable", function, A2GetVariable, GameObject | GameGroup | GameString, GameString | GameArray),
    GameOperator(GameString, "createSimpleTask", function, A2CreateSimpleTask, GameObject, GameArray),
    GameOperator(GameNothing, "setSimpleTaskDescription", function, A2SetSimpleTaskDescription, GameString, GameArray),
    GameOperator(GameNothing, "setSimpleTaskDestination", function, A2SetSimpleTaskDestination, GameString, GameArray),
    GameOperator(GameNothing, "setTaskState", function, A2SetTaskState, GameString, GameString),
    GameOperator(GameNothing, "setCurrentTask", function, A2SetCurrentTask, GameObject, GameString),
    GameOperator(GameNothing, "removeSimpleTask", function, A2NothingBinary, GameObject, GameString),
    GameOperator(GameString, "createDiaryRecord", function, A2CreateDiaryRecord, GameObject, GameArray),
    GameOperator(GameString, "createDiarySubject", function, A2NothingBinary, GameObject, GameArray),
    GameOperator(GameNothing, "selectDiarySubject", function, A2NothingBinary, GameObject, GameString),
    GameOperator(GameNothing, "kbAddTopic", function, A2KbAddTopic, GameObject, GameArray),
    GameOperator(GameNothing, "kbRemoveTopic", function, A2KbRemoveTopic, GameObject, GameString),
    GameOperator(GameBool, "kbHasTopic", function, A2KbHasTopic, GameObject, GameString),
    GameOperator(GameNothing, "kbTell", function, A2KbTell, GameObject, GameArray),
    GameOperator(GameBool, "kbWasSaid", function, A2KbWasSaid, GameObject, GameArray),
    GameOperator(GameBool, "kbAddDatabase", function, A2TrueBinary, GameObject, GameString),
    GameOperator(GameBool, "kbAddDatabaseTargets", function, A2TrueBinary, GameObject, GameString),
    GameOperator(GameNothing, "kbReact", function, A2NothingBinary, GameObject, GameArray),
    GameOperator(GameScalar, "execFSM", function, A2ExecFSM, GameVoid, GameString),
    GameOperator(GameNothing, "moveTo", function, A2MoveTo, GameObject, GameArray),
    GameOperator(GameAny, "getFSMVariable", function, A2GetFSMVariable, GameScalar, GameString),
    GameOperator(GameNothing, "setFSMVariable", function, A2SetFSMVariable, GameScalar, GameArray),
    GameOperator(GameAny, "getFSMVariable", function, A3GetFSMVariableDefault, GameAny, GameArray),
    GameOperator(GameNothing, "ppEffectAdjust", function, A2NothingBinary, GameString | GameScalar, GameArray),
    GameOperator(GameNothing, "ppEffectCommit", function, A2NothingBinary, GameString | GameScalar | GameArray, GameScalar),
    GameOperator(GameNothing, "ppEffectEnable", function, A2NothingBinary, GameString | GameScalar | GameArray, GameBool),
    GameOperator(GameNothing, "setMarkerPosLocal", function, MarkerSetPos, GameString, GameArray),
    GameOperator(GameNothing, "setMarkerTypeLocal", function, MarkerSetType, GameString, GameString),
    GameOperator(GameNothing, "setMarkerSizeLocal", function, MarkerSetSize, GameString, GameArray),
    GameOperator(GameNothing, "setMarkerColorLocal", function, MarkerSetColor, GameString, GameString),
    GameOperator(GameNothing, "setMarkerTextLocal", function, MarkerSetText, GameString, GameString),
    GameOperator(GameNothing, "setMarkerShapeLocal", function, MarkerSetShape, GameString, GameString),
    GameOperator(GameNothing, "setMarkerBrushLocal", function, MarkerSetBrush, GameString, GameString),
    GameOperator(GameNothing, "setMarkerDirLocal", function, MarkerSetDir, GameString, GameScalar),
    GameOperator(GameNothing, "setMarkerAlpha", function, A2NothingBinary, GameString, GameScalar),
    GameOperator(GameNothing, "setMarkerAlphaLocal", function, A2NothingBinary, GameString, GameScalar),
    GameOperator(GameArray, "nearEntities", function, A2NearEntities, GameArray | GameObject, GameArray | GameScalar),
    GameOperator(GameNothing, "setPosATL", function, A2SetPosATL, GameObject, GameArray),
    GameOperator(GameNothing, "playMoveNow", function, A2PlayMoveNow, GameObject, GameString),
    GameOperator(GameNothing, "playAction", function, A2NothingBinary, GameObject, GameString),
    GameOperator(GameNothing, "playActionNow", function, A2NothingBinary, GameObject, GameString),
    GameOperator(GameNothing, "switchAction", function, A2NothingBinary, GameObject, GameString),
    GameOperator(GameNothing, "glanceAt", function, A2NothingBinary, GameObjectOrArray, GameObject | GameArray),
    GameOperator(GameNothing, "forceSpeed", function, A2NothingBinary, GameObject, GameScalar),
    GameOperator(GameNothing, "setUnitAbility", function, A2NothingBinary, GameObject, GameScalar),
    GameOperator(GameNothing, "setUnitRank", function, A2NothingBinary, GameObject, GameString),
    GameOperator(GameNothing, "setVehicleLock", function, A2SetVehicleLock, GameObject, GameString),
    GameOperator(GameNothing, "setWaypointCompletionRadius", function, A2NothingBinary, GameArray, GameScalar),
    GameOperator(GameNothing, "synchronizeObjectsAdd", function, A2NothingBinary, GameObject, GameArray),
    GameOperator(GameNothing, "synchronizeObjectsRemove", function, A2NothingBinary, GameObject, GameArray),
    GameOperator(GameNothing, "disableConversation", function, A2NothingBinary, GameObject, GameBool),
    GameOperator(GameNothing, "setHit", function, A2NothingBinary, GameObject, GameArray),
    GameOperator(GameNothing, "addPublicVariableEventHandler", function, A2NothingBinary, GameString, GameString),
    GameOperator(GameNothing, "ctrlSetTooltip", function, A2NothingBinary, GameVoid, GameString),
    GameOperator(GameNothing, "progressSetPosition", function, A2NothingBinary, GameVoid, GameScalar),
    GameOperator(GameNothing, "setText", function, A2LocSetText, GameString, GameString),
    GameOperator(GameNothing, "setType", function, A2LocSetType, GameString, GameString),
    GameOperator(GameNothing, "setPosition", function, A2LocSetPosition, GameString, GameArray),
    GameOperator(GameNothing, "setSize", function, A2LocSetSize, GameString, GameArray),
    GameOperator(GameNothing, "setRectangular", function, A2NothingBinary, GameString, GameBool),
    GameOperator(GameNothing, "setDirection", function, A2NothingBinary, GameString, GameScalar),
    GameOperator(GameNothing, "moveInCargo", function, A2MoveInCargoIndex, GameObject, GameArray),
    GameOperator(GameNothing, "hideObject", function, A2NothingBinary, GameObject, GameBool),
    GameOperator(GameNothing, "forceWalk", function, A2NothingBinary, GameObject, GameBool),
    GameOperator(GameNothing, "say2D", function, A2NothingBinary, GameObject, GameString | GameArray),
    GameOperator(GameNothing, "ctrlSetScale", function, A2CtrlSetScale, GameScalar, GameScalar),
    GameOperator(GameNothing, "groupSelectUnit", function, A2NothingBinary, GameObject, GameArray),
    GameOperator(GameNothing, "setVehicleTiPars", function, A2NothingBinary, GameObject, GameArray),
    GameOperator(GameArray, "unitsBelowHeight", function, A2EmptyArrayBinary, GameGroup | GameArray, GameScalar),
    GameOperator(GameNothing, "addBackpack", function, A2AddBackpack, GameObject, GameString),
    GameOperator(GameNothing, "hcSetGroup", function, A2NothingBinary, GameObject, GameArray),
    GameOperator(GameNothing, "hcRemoveGroup", function, A2NothingBinary, GameObject, GameGroup),
    GameOperator(GameNothing, "cutText", function, A2CutTextLayer, GameScalar, GameArray),
    GameOperator(GameNothing, "cutRsc", function, A2CutRscLayer, GameScalar, GameArray),
    GameOperator(GameNothing, "cutObj", function, A2CutObjLayer, GameScalar, GameArray),
    GameOperator(GameNothing, "cutFadeOut", function, A2CutFadeOutLayer, GameScalar, GameScalar),
    GameOperator(GameNothing, "setVehicleInit", function, A2SetVehicleInit, GameObject, GameString),
    GameOperator(GameNothing, "attachTo", function, A2AttachTo, GameObject, GameArray),
    GameOperator(GameNothing, "copyWaypoints", function, A2NothingBinary, GameGroup, GameGroup),
    GameOperator(GameNothing, "setCurrentWaypoint", function, A2SetCurrentWaypoint, GameGroup, GameArray),
    GameOperator(GameScalar, "emptyPositions", function, A2EmptyPositions, GameObject, GameString),
    GameOperator(GameNothing, "useAudioTimeForMoves", function, A2NothingBinary, GameObject, GameBool),
    GameOperator(GameScalar, "displayAddEventHandler", function, A2ZeroBinary, GameAny, GameArray),
    GameOperator(GameNothing, "displayRemoveEventHandler", function, A2NothingBinary, GameAny, GameArray),
    GameOperator(GameScalar, "ctrlAddEventHandler", function, A2ZeroBinary, GameAny, GameArray),
    GameOperator(GameNothing, "ctrlRemoveEventHandler", function, A2NothingBinary, GameAny, GameArray),
    GameOperator(GameArray, "selectionPosition", function, A2SelectionPosition, GameObject, GameString | GameArray),
    GameOperator(GameNothing, "hcSelectGroup", function, A2NothingBinary, GameObject, GameArray),
    GameOperator(GameNothing, "lnbSetColumnsPos", function, A2NothingBinary, GameScalar, GameArray),
    GameOperator(GameAny, "do", function, A2WithDo, GameString, GameString),
    GameOperator(GameNothing, "setVelocityTransformation", function, A2SetVelocityTransformation, GameObject, GameArray),
    GameOperator(GameScalar, "knowsAbout", function, A2SideKnowsAbout, GameSide, GameObject),
    GameOperator(GameArray, "weaponDirection", function, A2WeaponDirection, GameObject, GameString),
    GameOperator(GameScalar, "addGroupIcon", function, A2ZeroBinary, GameGroup, GameArray),
    GameOperator(GameNothing, "setGroupIconParams", function, A2NothingBinary, GameGroup, GameArray),
    GameOperator(GameNothing, "enableIRLasers", function, A2NothingBinary, GameObject | GameGroup, GameBool),
    GameOperator(GameNothing, "respawnVehicle", function, A2NothingBinary, GameObject, GameArray),
    GameOperator(GameNothing, "ctrlMapAnimAdd", function, A2NothingBinary, GameAny, GameArray),
    GameOperator(GameNothing, "setVehicleArmor", function, A2NothingBinary, GameObject, GameScalar),
    GameOperator(GameNothing, "lockCargo", function, A2NothingBinary, GameObject, GameBool | GameArray),
    GameOperator(GameNothing, "lockDriver", function, A2NothingBinary, GameObject, GameBool),
    GameOperator(GameNothing, "lockTurret", function, A2NothingBinary, GameObject, GameArray),
};
} // namespace

void RegisterA2WorldCommands(); // GameStateExtA2World.cpp

INIT_MODULE(GameStateExtA2, 3)
{
    RegisterA2WorldCommands();
    for (const GameNular& n : A2Nular)
        GGameState.NewNularOp(n);
    for (const GameFunction& f : A2Unary)
        GGameState.NewFunction(f);
    for (const GameOperator& o : A2Binary)
        GGameState.NewOperator(o);
};
