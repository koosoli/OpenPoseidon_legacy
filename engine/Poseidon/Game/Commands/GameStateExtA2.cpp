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

#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/AICenter.hpp>
#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Modules/Modules.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Poseidon;

bool PreprocessScriptFile(const char* name, std::string& out); // GameStateExtWorldConfig.cpp

void Arma2RuntimeSimulate(); // called from World::SimulateScripts

// CWA commands reused below (defined in the other GameStateExt*.cpp files).
GameValue ShowHint(const GameState* state, GameValuePar oper1);
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
    // kb: topic -> parsed bikb, per speaker
    std::map<std::string, std::shared_ptr<ParamFile>> bikbFiles;                 // file -> parsed
    std::map<const void*, std::map<std::string, std::string>> topics;            // speaker -> topic -> file
    std::map<std::string, KbSentence> said;                                      // speaker|receiver|topic|id
    int soundSerial = 0;
    // FSMs
    std::vector<std::unique_ptr<Fsm>> fsms;
    int nextFsm = 0;
};

Runtime& RT()
{
    static Runtime rt;
    const float now = Now();
    const std::string mission = (const char*)Glob.header.filenameReal;
    if (mission != rt.mission || now + 0.001f < rt.lastTime)
    {
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
            const auto it = vars.find(Lower((const char*)name));
            return it != vars.end() ? it->second : fallback;
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
        if (args.Size() > 1 && args[1].GetType() == GameString && task->name.empty())
            task->name = (const char*)RString(args[1]);
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
    LOG_INFO(Mission, "Arma 2: task '{}' -> {}", task->name, upper);
    if (upper == "SUCCEEDED" || upper == "FAILED" || upper == "CANCELED")
    {
        const char* word = upper == "SUCCEEDED" ? "Task accomplished" : upper == "FAILED" ? "Task failed" : "Task canceled";
        ShowHint(state, GameValue(RString((std::string(word) + ":\n" + task->name).c_str())));
    }
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
    return state->CreateGameValue(GameArray);
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
            RT().currentTask[obj] = index + 1;
            if (obj == GWorld->GetRealPlayer())
                ShowHint(state, GameValue(RString(("Current task:\n" + RT().tasks[index].name).c_str())));
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
        if (entry.Size() > 0 && entry[0].GetType() == GameString)
            LOG_INFO(Mission, "Arma 2: diary '{}': {}", (const char*)RString(args[0]),
                     (const char*)RString(entry[0]));
    }
    return GameValue(RString(("::DIARY::" + std::to_string(++RT().diaryRecords)).c_str()));
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

GameValue A2KbTell(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Object* who = GetObject(oper1);
    const GameArrayType& args = oper2;
    if (!who || args.Size() < 3 || args[1].GetType() != GameString || args[2].GetType() != GameString)
        return NOTHING;
    Object* to = args[0].GetType() == GameObject ? GetObject(args[0]) : nullptr;
    const std::string topic = (const char*)RString(args[1]);
    const std::string id = (const char*)RString(args[2]);
    std::string file;
    {
        const auto& mine = RT().topics[who];
        const auto it = mine.find(Lower(topic));
        if (it != mine.end())
            file = it->second;
    }
    float duration = 2.0f;
    std::string shown;
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
                    std::string wave = (const char*)(RStringB)(*speech)[0];
                    std::string bank = wave;
                    while (!bank.empty() && (bank[0] == '\\' || bank[0] == '/'))
                        bank.erase(0, 1);
                    if (!bank.empty() && QIFStreamB::FileExist(bank.c_str()))
                    {
                        const RString cls = SpeechSoundClass(wave.c_str(), shown.c_str());
                        if (cls.GetLength() > 0)
                            ObjSay(state, oper1, GameValue(cls));
                        duration = std::max(duration, 0.07f * static_cast<float>(shown.size()) + 1.0f);
                    }
                }
            }
        }
    }
    if (shown.empty())
        shown = id;
    duration = std::max(duration, 0.06f * static_cast<float>(shown.size()) + 1.0f);
    ObjGroupChat(state, oper1, GameValue(RString(shown.c_str())));
    KbSentence s;
    s.start = Now();
    s.end = s.start + duration;
    RT().said[SaidKey(who, to, topic, id)] = s;
    RT().said[SaidKey(who, nullptr, topic, id)] = s;
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
    FsmRun(fsm, EntryText(st, "init"));
    for (const std::string& f : fsm.finals)
        if (_stricmp(f.c_str(), name.c_str()) == 0)
            fsm.done = true;
    if (!st)
        fsm.done = true;
}

void FsmStep(Fsm& fsm)
{
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

GameValue A2ExecFSM(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    const std::string file = (const char*)RString(oper2);
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
    fsm->vars.VarSet("_this", oper1.GetNil() ? GameValue() : oper1, true);
    const int handle = fsm->handle;
    RT().fsms.push_back(std::move(fsm));
    return GameValue(static_cast<float>(handle));
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
        fsm->vars.VarGet(RString(oper2), v);
    return v;
}

GameValue A2SetFSMVariable(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Fsm* fsm = FindFsm(oper1);
    const GameArrayType& args = oper2;
    if (fsm && args.Size() >= 2 && args[0].GetType() == GameString)
        fsm->vars.VarSet(RString(args[0]), args[1]);
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

GameValue A2VehicleVarName(const GameState*, GameValuePar)
{
    return GameValue(RString(""));
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

GameValue A2EmptyArrayBinary(const GameState* state, GameValuePar, GameValuePar)
{
    return state->CreateGameValue(GameArray);
}

GameValue A2PpEffectCreate(const GameState*, GameValuePar)
{
    static float next = 0;
    return GameValue(next += 1.0f);
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
} // namespace

// Runs the scripted FSMs one transition per simulation step (World::SimulateScripts).
void Arma2RuntimeSimulate()
{
    if (Arma1Compat::Generation() != 2 || !GWorld || !GWorld->GetGameState())
        return;
    Runtime& rt = RT();
    for (size_t i = 0; i < rt.fsms.size(); i++) // FSMs may start FSMs
        FsmStep(*rt.fsms[i]);
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
    GameNular(GameScalar, "diag_fps", A2One),
    GameNular(GameBool, "isMultiplayer", A2False),
    GameNular(GameBool, "isDedicated", A2False),
    GameNular(GameScalar, "serverTime", GameTime),
    GameNular(GameSide, "playerSide", A2PlayerSide),
    GameNular(GameScalar, "safeZoneX", A2Zero),
    GameNular(GameScalar, "safeZoneY", A2Zero),
    GameNular(GameScalar, "safeZoneW", A2SafeZoneW),
    GameNular(GameScalar, "safeZoneH", A2SafeZoneW),
    GameNular(GameScalar, "safeZoneXAbs", A2Zero),
    GameNular(GameScalar, "safeZoneWAbs", A2SafeZoneW),
    GameNular(GameObject, "locationNull", A2ObjNull),
    GameNular(GameNothing, "disableSerialization", A2NothingNular),
    GameNular(GameNothing, "endLoadingScreen", A2NothingNular),
    GameNular(GameNothing, "clearRadio", A2NothingNular),
    GameNular(GameBool, "visibleMap", A2False),
    GameNular(GameBool, "savingEnabled", A2True),
    GameNular(GameBool, "teamSwitchEnabled", A2False),
    GameNular(GameScalar, "viewDistance", A2One),
};

const GameFunction A2Unary[] = {
    GameFunction(GameNothing, "textLogFormat", A2NothingUnary, GameArray),
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
    GameFunction(GameScalar, "ppEffectCreate", A2PpEffectCreate, GameArray),
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
    GameFunction(GameBool, "completedFSM", A2CompletedFSM, GameScalar),
    GameFunction(GameString, "lifeState", A2LifeState, GameObject),
    GameFunction(GameString, "vehicleVarName", A2VehicleVarName, GameObject),
    GameFunction(GameArray, "nearestObjects", A2NearestObjects, GameArray),
    GameFunction(GameArray, "getPosATL", A2GetPosATL, GameObject),
    GameFunction(GameString, "currentWeapon", ObjGetPrimaryWeapon, GameObject),
    GameFunction(GameObject, "effectiveCommander", ObjCommander, GameObject),
    GameFunction(GameString, "faction", A2Faction, GameObject),
    GameFunction(GameArray, "synchronizedObjects", A2EmptyArrayUnary, GameObject),
    GameFunction(GameArray, "actionKeysNames", A2EmptyArrayUnary, GameString | GameArray),
    GameFunction(GameNothing, "hcRemoveAllGroups", A2NothingUnary, GameObject),
    GameFunction(GameBool, "hcShownBar", A2TrueUnary, GameAny),
};

const GameOperator A2Binary[] = {
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
    GameOperator(GameAny, "getFSMVariable", function, A2GetFSMVariable, GameScalar, GameString),
    GameOperator(GameNothing, "setFSMVariable", function, A2SetFSMVariable, GameScalar, GameArray),
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
    GameOperator(GameNothing, "setGroupIconsVisible", function, A2NothingBinary, GameVoid, GameArray),
    GameOperator(GameNothing, "disableConversation", function, A2NothingBinary, GameObject, GameBool),
    GameOperator(GameNothing, "setHit", function, A2NothingBinary, GameObject, GameArray),
    GameOperator(GameNothing, "addPublicVariableEventHandler", function, A2NothingBinary, GameString, GameString),
    GameOperator(GameNothing, "ctrlSetTooltip", function, A2NothingBinary, GameVoid, GameString),
    GameOperator(GameNothing, "progressSetPosition", function, A2NothingBinary, GameVoid, GameScalar),
};
} // namespace

INIT_MODULE(GameStateExtA2, 3)
{
    for (const GameNular& n : A2Nular)
        GGameState.NewNularOp(n);
    for (const GameFunction& f : A2Unary)
        GGameState.NewFunction(f);
    for (const GameOperator& o : A2Binary)
        GGameState.NewOperator(o);
};
