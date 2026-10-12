
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Game/Scripting/Scripts.hpp>
#include <functional>
#include <string>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/World.hpp>
#include <ctype.h>
#include <float.h>
#include <stdio.h>
#include <string.h>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/platform.hpp>

namespace Poseidon
{
RString FindScript(RString name);
}

namespace Poseidon
{
#define DIAG_DEBUG 0 // script debug level (0..90)

#if _MSC_VER >= 1300
#define SCRIPTS_DEBUGGING 0
#else
#define SCRIPTS_DEBUGGING 0
#endif

#if SCRIPTS_DEBUGGING
#import "dte.olb" raw_interfaces_only named_guids
static EnvDTE::DebuggerPtr GDebugger = nullptr;
static int nScripts = 0;

static EnvDTE::DebuggerPtr CreateDebugger()
{
    IUnknownPtr pUnk;
    GetActiveObject(EnvDTE::CLSID_DTE, nullptr, &pUnk);

    if (pUnk == nullptr)
        return nullptr;
    EnvDTE::_DTEPtr pDTE = pUnk;
    if (pDTE == nullptr)
        return nullptr;

    EnvDTE::DebuggerPtr debugger;
    if (!SUCCEEDED(pDTE->get_Debugger(&debugger)))
        return nullptr;

    EnvDTE::dbgDebugMode mode;
    if (!SUCCEEDED(debugger->get_CurrentMode(&mode)))
        return nullptr;
    if (mode == EnvDTE::dbgDesignMode)
        return nullptr;

    return debugger;
}

void InitDebugger()
{
    if (nScripts++ == 0)
    {
        CoInitialize(nullptr);
        GDebugger = CreateDebugger();
    }
}
void DoneDebugger()
{
    if (--nScripts == 0)
    {
        GDebugger = nullptr;
        CoUninitialize();
    }
}
#endif

Script::Script()
{
    _baseTime = Glob.time;
    _currentLine = 0;

    _time = 0;
    _suspendedUntil = 0;
    _maxLinesAtOnce = 100;

#if SCRIPTS_DEBUGGING
    InitDebugger();
#endif
}

static void ReadLine(QIStream& in, char* buffer, int bufferSize)
{
    char* p = buffer;
    char* endP = buffer + bufferSize - 1;
    char c = in.get();
    while (!in.eof() && c == ' ')
    {
        c = in.get();
    }
    while (!in.eof() && c != '\r' && c != '\n')
    {
        if (p < endP)
        {
            *(p++) = c;
        }
        c = in.get();
    }
    *p = 0;
    while (!in.eof() && (c == '\r' || c == '\n'))
    {
        c = in.get();
    }
    if (!in.eof())
    {
        in.unget();
    }
}

} // namespace Poseidon
bool OpenScript(QIFStreamB& in, RString name)
{
    using namespace Poseidon;
    RString fullname;
    if (name[0] == '\\')
    {
        fullname = (const char*)name + 1;
        if (!QIFStreamB::FileExist(fullname))
        {
            Foundation::WarningMessage("Script %s not found", (const char*)name);
            fullname = "";
        }
    }
    else
    {
        fullname = Poseidon::FindScript(name);
    }

#if DIAG_DEBUG >= 20
    LOG_DEBUG(Script, "Load {}", (const char*)name);
#endif

    in.AutoOpen(fullname);
    return !in.fail();
}
namespace Poseidon
{

Script::Script(RString name, GameValuePar argument, int maxLines)
{
    _argument = argument;
    _baseTime = Glob.time;
    _currentLine = 0;
    _maxLinesAtOnce = maxLines;

    _time = 0;
    _suspendedUntil = 0;
    _name = name;

    QIFStreamB in;
    OpenScript(in, name);
    while (!in.eof() && !in.fail())
    {
        char buffer[4096];
        ReadLine(in, buffer, sizeof(buffer));
        ProcessLine(buffer);
    }
#if SCRIPTS_DEBUGGING
    InitDebugger();
#endif
}

Script::Script(const AutoArray<RString>& lines, GameValuePar argument, int maxLines)
{
    _argument = argument;
    _baseTime = Glob.time;
    _currentLine = 0;
    _maxLinesAtOnce = maxLines;

    _time = 0;
    _suspendedUntil = 0;
    _name = "Constructed";

    for (int i = 0; i < lines.Size(); i++)
    {
        ProcessLine(lines[i]);
    }
#if SCRIPTS_DEBUGGING
    InitDebugger();
#endif
}

Script::~Script()
{
#if SCRIPTS_DEBUGGING
    DoneDebugger();
#endif
}

static void Trim(RString& str)
{
    int n = str.GetLength();
    int beg = 0, end = n;
    for (int i = 0; i < n; i++)
    {
        if (!isspace(str[i]))
        {
            beg = i;
            break;
        }
    }
    for (int i = n - 1; i >= 0; i--)
    {
        if (!isspace(str[i]))
        {
            end = i + 1;
            break;
        }
    }
    str = str.Substring(beg, end);
}

void Script::ProcessLine(const char* line)
{
    const char* pos = line;
    while (*pos > 0 && isspace(*pos))
    {
        pos++;
    }

    static RString trueCond("");
    static RString emptyCommand("");
    static RString noSuspend("");

    switch (*pos)
    {
        case 0:   // empty line
        case ';': // comment
            break;
        case '#': // label
        {
            int i = _labels.Add();
            _labels[i].name = pos + 1;
            _labels[i].line = _lines.Size();
            Trim(_labels[i].name);
        }
        break;
        case '&': // time wait condition (absolute)
        {
            const char* time = pos + 1;
            int i = _lines.Add();
            _lines[i].waitUntil = trueCond;
            _lines[i].suspendUntil = time;
            _lines[i].condition = trueCond;
            _lines[i].statement = emptyCommand;
        }
        break;
        case '~': // time wait condition (relative)
        {
            const char* time = pos + 1;
            char buf[256];
            snprintf(buf, sizeof(buf), "__waituntil = _time+(%s)", time);

            int i = _lines.Add();
            _lines[i].waitUntil = trueCond;
            _lines[i].suspendUntil = noSuspend;
            _lines[i].condition = trueCond;
            _lines[i].statement = buf;

            i = _lines.Add();
            _lines[i].waitUntil = trueCond;
            _lines[i].suspendUntil = "__waituntil";
            _lines[i].condition = trueCond;
            _lines[i].statement = emptyCommand;
        }
        break;
        case '@': // wait condition
        {
            int i = _lines.Add();
            const char* until = pos + 1;
            _lines[i].waitUntil = until;
            _lines[i].suspendUntil = noSuspend;
            _lines[i].condition = trueCond;
            _lines[i].statement = emptyCommand;
        }
        break;
        case '?': // condition : statement
        {
            const char* field1 = pos + 1;
            while (isspace(*field1))
            {
                field1++;
            }
            const char* field2 = strchr(field1, ':');
            if (!field2)
            {
                RptF("Only one field in line \"%s\".", line);
                break;
            }
            const char* field1End = field2;
            field2++;

            while (isspace(*field2))
            {
                field2++;
            }
            int i = _lines.Add();
            _lines[i].waitUntil = trueCond;
            _lines[i].suspendUntil = noSuspend;
            _lines[i].condition = *field1 ? RString(field1, field1End - field1) : RString((const char*)trueCond);
            _lines[i].statement = field2;
        }
        break;
        default:
        {
            const char* field1 = pos;
            while (isspace(*field1))
            {
                field1++;
            }
            int i = _lines.Add();
            _lines[i].waitUntil = trueCond;
            _lines[i].suspendUntil = noSuspend;
            _lines[i].condition = trueCond;
            _lines[i].statement = field1;
        }
        break;
    }
}

LSError ScriptLine::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(ar.Serialize("waitUntil", waitUntil, 1, "true"))
    PARAM_CHECK(ar.Serialize("suspendUntil", suspendUntil, 1, "0"))
    PARAM_CHECK(ar.Serialize("condition", condition, 1, "true"))
    PARAM_CHECK(ar.Serialize("statement", statement, 1))
    return LSOK;
}

LSError ScriptLabel::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(ar.Serialize("name", name, 1))
    PARAM_CHECK(ar.Serialize("line", line, 1, 0))
    return LSOK;
}

LSError Script::Serialize(ParamArchive& ar)
{
    if (!GWorld->GetGameState())
    {
        LOG_ERROR(Script, "Error: cannot load script - GWorld->GetGameState() == nullptr");
        return LSStructure;
    }

    if (ar.GetArVersion() >= 4)
    {
        void* old = ar.GetParams();
        ar.SetParams(GWorld->GetGameState());
        PARAM_CHECK(ar.Serialize("argument", _argument, 4))
        ar.SetParams(old);
    }
    else
    {
        if (ar.IsLoading())
        {
            OLink<Object> veh;
            PARAM_CHECK(ar.SerializeRef("Vehicle", veh, 1))
            _argument = GameValueExt(veh.GetLink());
        }
    }
    PARAM_CHECK(ar.Serialize("baseTime", _baseTime, 1))
    PARAM_CHECK(ar.Serialize("currentLine", _currentLine, 1, 0))
    PARAM_CHECK(ar.Serialize("Lines", _lines, 1))
    PARAM_CHECK(ar.Serialize("Labels", _labels, 1))
    PARAM_CHECK(ar.Serialize("Name", _name, 1, RString()))
    PARAM_CHECK(ar.Serialize("maxLinesAtOnce", _maxLinesAtOnce, 1, -1))

    void* old = ar.GetParams();
    ar.SetParams(GWorld->GetGameState());
    PARAM_CHECK(ar.Serialize("Vars", _vars._vars, 1))
    ar.SetParams(old);

    LSError err = ar.Serialize("time", _time, 1);
    if (err != LSOK)
    {
        if (ar.IsSaving())
        {
            return err;
        }
        else
        {
            _time = VarGet("_time");
        }
    }
    PARAM_CHECK(ar.Serialize("suspendedUntil", _suspendedUntil, 1, -FLT_MAX))

    return LSOK;
}

static Script* CurrentScript = nullptr;

bool Script::IsTerminated() const
{
    return _currentLine >= _lines.Size();
}

bool Script::SimulateBody()
{
#if SCRIPTS_DEBUGGING
    if (GDebugger)
    {
        EnvDTE::Breakpoints* breakpoints = nullptr;
        GDebugger->get_Breakpoints(&breakpoints);
        long n = 0;
        breakpoints->get_Count(&n);
        for (long i = 0; i < n; i++)
        {
            VARIANT index;
            index.vt = VT_I4;
            index.lVal = i;

            EnvDTE::Breakpoint* breakpoint = nullptr;
            breakpoints->Item(index, &breakpoint);
            BSTR filename;
            breakpoint->get_File(&filename);
        }
    }
#endif

    GameState* gstate = GWorld->GetGameState();
    gstate->VarSetLocal("_this", _argument, true);
// if simulate body takes too long, interrupt it
#if DIAG_DEBUG >= 10
    DWORD timeStart = Foundation::GlobalTickCount();
#endif
    int cntExecute = 0;
    int maxExecute = _maxLinesAtOnce > 0 ? _maxLinesAtOnce : 100;
    while (_currentLine < _lines.Size())
    {
        ScriptLine& line = _lines[_currentLine];
        if (line.suspendUntil.GetLength() > 0)
        {
            _suspendedUntil = gstate->Evaluate(line.suspendUntil);
            if (_time < _suspendedUntil)
            {
                break;
            }
        }
        if (line.waitUntil.GetLength() > 0 && !gstate->EvaluateBool(line.waitUntil))
        {
            break;
        }
        _currentLine++;
        if (
            // empty condition is considered true
            (line.condition.GetLength() == 0 || gstate->EvaluateBool(line.condition))
            // empty command is not executed
            && line.statement.GetLength() > 0)
        {
#if DIAG_DEBUG >= 20
            LOG_DEBUG(Script, "Execute {}", (const char*)line.statement);
#endif
            gstate->Execute(line.statement);
            _time = gstate->VarGet("_time");
        }
        if (++cntExecute >= maxExecute)
        {
#if DIAG_DEBUG >= 10
            DWORD time = Foundation::GlobalTickCount();
            LOG_DEBUG(Script, "Script interrupted after {} ms", time - timeStart);
#endif
            break;
        }
    }
#if DIAG_DEBUG >= 10
    if (cntExecute > 0)
    {
        DWORD time = Foundation::GlobalTickCount();
        LOG_DEBUG(Script, "Script paused after {} ms", time - timeStart);
    }
#endif
    return _currentLine >= _lines.Size();
}

bool Script::Simulate(float deltaT)
{
    if (CurrentScript)
    {
        return false;
    }
    bool ret = false;
    if (_time >= _suspendedUntil)
    {
        CurrentScript = this;
        GameState* gstate = GWorld->GetGameState();
        gstate->BeginContext(&_vars);
        gstate->VarSet("_time", _time); // make sure there is such variable
        ret = SimulateBody();
        _time = gstate->VarGet("_time");
        gstate->EndContext();
        CurrentScript = nullptr;
    }
    _time += deltaT;
    return ret;
}

bool Script::OnSimulate()
{
    if (CurrentScript)
    {
        return false;
    }
    bool ret = false;
    _time = Glob.time - _baseTime;
    if (_time >= _suspendedUntil)
    {
        CurrentScript = this;
        GameState* gstate = GWorld->GetGameState();
        gstate->BeginContext(&_vars);
        gstate->VarSet("_time", _time);
        ret = SimulateBody();
        _time = gstate->VarGet("_time");
        _baseTime = Glob.time - _time;
        gstate->EndContext();
        CurrentScript = nullptr;
    }
    return ret;
}

GameValue Script::VarGet(const char* name) const
{
    GameState* gstate = GWorld->GetGameState();
    gstate->BeginContext(const_cast<GameVarSpace*>(&_vars));
    GameValuePar retval = gstate->VarGet(name);
    gstate->EndContext();
    return retval;
}

void Script::VarSet(const char* name, GameValuePar value, bool readOnly)
{
    GameState* gstate = GWorld->GetGameState();
    gstate->BeginContext(&_vars);
    gstate->VarSet(name, value, readOnly);
    gstate->EndContext();
}

void Script::GoTo(RString label)
{
    for (int i = 0; i < _labels.Size(); i++)
    {
        if (stricmp(_labels[i].name, label) == 0)
        {
            _currentLine = _labels[i].line;
            return;
        }
    }
    LOG_DEBUG(Script, "Unknown label {}.", (const char*)label);
    Exit();
}

void Script::Exit()
{
    _currentLine = _lines.Size();
}

#define NOTHING GameValue()

} // namespace Poseidon
GameValue ScriptExecute(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    using namespace Poseidon;
    DoAssert(oper2.GetType() == GameString) Script* script = new Script(oper2, oper1);
    GWorld->AddScript(script);
    return NOTHING;
}
namespace Poseidon
{

} // namespace Poseidon
GameValue ScriptExit(const GameState* state)
{
    using namespace Poseidon;
    if (CurrentScript)
    {
        CurrentScript->Exit();
    }
    return NOTHING;
}
namespace Poseidon
{

} // namespace Poseidon
GameValue ScriptGoto(const GameState* state, GameValuePar oper1)
{
    using namespace Poseidon;
    DoAssert(oper1.GetType() == GameString) if (CurrentScript)
    {
        CurrentScript->GoTo(oper1);
    }
    return NOTHING;
}

// ---------------------------------------------------------------------------
// Scheduled SQF for Arma 1 content: spawn / execVM / terminate / scriptDone.
// ---------------------------------------------------------------------------
#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <map>
#include <string>
#include <vector>

namespace Poseidon
{
namespace
{
std::map<int, LLink<Script>>& ScriptHandles()
{
    static std::map<int, LLink<Script>> handles;
    return handles;
}

int NextScriptHandle()
{
    static int next = 0;
    // Drop finished/destroyed scripts so the table stays small.
    auto& handles = ScriptHandles();
    for (auto it = handles.begin(); it != handles.end();)
    {
        if (!it->second || it->second->IsTerminated())
            it = handles.erase(it);
        else
            ++it;
    }
    return ++next;
}

// Removes // and /* */ comments outside string literals.
std::string StripSqfComments(const std::string& code)
{
    std::string out;
    out.reserve(code.size());
    char quote = 0;
    for (size_t i = 0; i < code.size(); i++)
    {
        const char c = code[i];
        if (quote)
        {
            out.push_back(c);
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            quote = c;
            out.push_back(c);
            continue;
        }
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '/')
        {
            while (i < code.size() && code[i] != '\n')
                i++;
            out.push_back('\n');
            continue;
        }
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < code.size() && !(code[i] == '*' && code[i + 1] == '/'))
                i++;
            i++;
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// Splits SQF into top-level statements (';' outside strings and brackets).
std::vector<std::string> SplitSqfStatements(const std::string& code)
{
    std::vector<std::string> statements;
    std::string current;
    int depth = 0;
    char quote = 0;
    for (const char c : code)
    {
        if (quote)
        {
            current.push_back(c);
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'')
            quote = c;
        else if (c == '{' || c == '[' || c == '(')
            depth++;
        else if ((c == '}' || c == ']' || c == ')') && depth > 0)
            depth--;
        else if ((c == ';' || c == ',') && depth == 0)
        {
            statements.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    statements.push_back(current);
    std::vector<std::string> trimmed;
    for (std::string& s : statements)
    {
        size_t b = 0, e = s.size();
        while (b < e && isspace(static_cast<unsigned char>(s[b])))
            b++;
        while (e > b && isspace(static_cast<unsigned char>(s[e - 1])))
            e--;
        if (e > b)
            trimmed.push_back(s.substr(b, e - b));
    }
    return trimmed;
}

bool StartsWithWord(const std::string& s, const char* word, std::string& rest)
{
    const size_t n = strlen(word);
    if (s.size() <= n || _strnicmp(s.c_str(), word, n) != 0)
        return false;
    const char next = s[n];
    if (isalnum(static_cast<unsigned char>(next)) || next == '_')
        return false;
    size_t b = n;
    while (b < s.size() && isspace(static_cast<unsigned char>(s[b])))
        b++;
    rest = s.substr(b);
    return true;
}

// "{ body }" -> "body" when the braces enclose the whole text.
bool UnwrapBraces(const std::string& s, std::string& body)
{
    if (s.size() < 2 || s.front() != '{' || s.back() != '}')
        return false;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++)
    {
        const char c = s[i];
        if (quote)
        {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'')
            quote = c;
        else if (c == '{')
            depth++;
        else if (c == '}' && --depth == 0 && i + 1 != s.size())
            return false;
    }
    body = s.substr(1, s.size() - 2);
    return true;
}

// The balanced {..} / (..) / [..] starting at s[pos] (pos at the opening bracket); its inner
// text in `inner`, pos moved past the closing bracket.
bool TakeBracketed(const std::string& s, size_t& pos, char open, char close, std::string& inner)
{
    while (pos < s.size() && isspace(static_cast<unsigned char>(s[pos])))
        pos++;
    if (pos >= s.size() || s[pos] != open)
        return false;
    int depth = 0;
    char quote = 0;
    for (size_t i = pos; i < s.size(); i++)
    {
        const char c = s[i];
        if (quote)
        {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'')
            quote = c;
        else if (c == open)
            depth++;
        else if (c == close && --depth == 0)
        {
            inner = s.substr(pos + 1, i - pos - 1);
            pos = i + 1;
            return true;
        }
    }
    return false;
}

bool TakeWord(const std::string& s, size_t& pos, const char* word)
{
    while (pos < s.size() && isspace(static_cast<unsigned char>(s[pos])))
        pos++;
    const size_t n = strlen(word);
    if (s.size() - pos < n || _strnicmp(s.c_str() + pos, word, n) != 0)
        return false;
    if (pos + n < s.size() && (isalnum(static_cast<unsigned char>(s[pos + n])) || s[pos + n] == '_'))
        return false;
    pos += n;
    return true;
}

bool AtEnd(const std::string& s, size_t pos)
{
    while (pos < s.size() && isspace(static_cast<unsigned char>(s[pos])))
        pos++;
    return pos >= s.size();
}

// Does the code contain sleep or waitUntil (outside strings, as whole words)?
bool HasSuspension(const std::string& s)
{
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++)
    {
        const char c = s[i];
        if (quote)
        {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            quote = c;
            continue;
        }
        if (i > 0 && (isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_'))
            continue;
        size_t p = i;
        if (TakeWord(s, p, "sleep") || TakeWord(s, p, "waitUntil"))
            return true;
    }
    return false;
}

struct SqfCase
{
    std::vector<std::string> values; // empty: default
    std::string code;
};

struct SqfControl
{
    enum Kind
    {
        While,
        If,
        For,
        Switch,
        ExitWith,
        ForEach
    } kind = While;
    std::string condition, body, elseBody, init, step;
    std::vector<SqfCase> cases; // Switch; condition holds the switched value
};

// "case v: {code}" / "case v" (falls through) / "default {code}"
bool ParseSwitchBody(const std::string& body, std::vector<SqfCase>& cases)
{
    std::vector<std::string> pending;
    for (const std::string& statement : SplitSqfStatements(body))
    {
        std::string rest;
        if (StartsWithWord(statement, "default", rest))
        {
            std::string code;
            if (!UnwrapBraces(rest, code))
                return false;
            cases.push_back({{}, code});
            continue;
        }
        if (!StartsWithWord(statement, "case", rest))
            return false;
        // the ':' at bracket depth 0 outside strings separates value and code
        int depth = 0;
        char quote = 0;
        size_t colon = std::string::npos;
        for (size_t i = 0; i < rest.size() && colon == std::string::npos; i++)
        {
            const char c = rest[i];
            if (quote)
            {
                if (c == quote)
                    quote = 0;
                continue;
            }
            if (c == '"' || c == '\'')
                quote = c;
            else if (c == '(' || c == '[' || c == '{')
                depth++;
            else if (c == ')' || c == ']' || c == '}')
                depth--;
            else if (c == ':' && depth == 0)
                colon = i;
        }
        if (colon == std::string::npos)
        {
            pending.push_back(rest); // falls through to the next case with code
            continue;
        }
        std::string code;
        size_t b = colon + 1;
        while (b < rest.size() && isspace(static_cast<unsigned char>(rest[b])))
            b++;
        if (!UnwrapBraces(rest.substr(b), code))
            return false;
        pending.push_back(rest.substr(0, colon));
        cases.push_back({pending, code});
        pending.clear();
    }
    return true;
}

// while {c} do {b} | if (c) then {a} [else {b}] | for "_v" from a to b [step s] do {b} |
// for [{i},{c},{s}] do {b}
bool ParseSqfControl(const std::string& s, SqfControl& out)
{
    size_t pos = 0;
    if (TakeWord(s, pos, "while"))
    {
        out.kind = SqfControl::While;
        return TakeBracketed(s, pos, '{', '}', out.condition) && TakeWord(s, pos, "do") &&
               TakeBracketed(s, pos, '{', '}', out.body) && AtEnd(s, pos);
    }
    pos = 0;
    if (TakeWord(s, pos, "if"))
    {
        size_t probe = pos;
        std::string c, b;
        if (TakeBracketed(s, probe, '(', ')', c) && TakeWord(s, probe, "exitWith") &&
            TakeBracketed(s, probe, '{', '}', b) && AtEnd(s, probe))
        {
            out.kind = SqfControl::ExitWith;
            out.condition = c;
            out.body = b;
            return true;
        }
        out.kind = SqfControl::If;
        if (!TakeBracketed(s, pos, '(', ')', out.condition) || !TakeWord(s, pos, "then") ||
            !TakeBracketed(s, pos, '{', '}', out.body))
            return false;
        if (TakeWord(s, pos, "else") && !TakeBracketed(s, pos, '{', '}', out.elseBody))
            return false;
        return AtEnd(s, pos);
    }
    pos = 0;
    {
        // {body} forEach list
        std::string b;
        size_t probe = 0;
        if (TakeBracketed(s, probe, '{', '}', b) && TakeWord(s, probe, "forEach"))
        {
            out.kind = SqfControl::ForEach;
            out.body = b;
            out.condition = s.substr(probe); // the list expression
            return !AtEnd(s, probe);
        }
    }
    pos = 0;
    if (TakeWord(s, pos, "switch"))
    {
        out.kind = SqfControl::Switch;
        return TakeBracketed(s, pos, '(', ')', out.condition) && TakeWord(s, pos, "do") &&
               TakeBracketed(s, pos, '{', '}', out.body) && AtEnd(s, pos) && ParseSwitchBody(out.body, out.cases);
    }
    pos = 0;
    if (!TakeWord(s, pos, "for"))
        return false;
    out.kind = SqfControl::For;
    std::string inner;
    if (TakeBracketed(s, pos, '[', ']', inner))
    {
        const std::vector<std::string> parts = SplitSqfStatements(inner); // splits at ','
        std::string a, b, c;
        if (parts.size() != 3 || !UnwrapBraces(parts[0], a) || !UnwrapBraces(parts[1], b) ||
            !UnwrapBraces(parts[2], c))
            return false;
        out.init = a;
        out.condition = b;
        out.step = c;
        return TakeWord(s, pos, "do") && TakeBracketed(s, pos, '{', '}', out.body) && AtEnd(s, pos);
    }
    // for "_v" from a to b [step s] do {body}
    while (pos < s.size() && isspace(static_cast<unsigned char>(s[pos])))
        pos++;
    if (pos >= s.size() || s[pos] != '"')
        return false;
    const size_t close = s.find('"', pos + 1);
    if (close == std::string::npos)
        return false;
    const std::string var = s.substr(pos + 1, close - pos - 1);
    pos = close + 1;
    if (!TakeWord(s, pos, "from"))
        return false;
    const auto until = [&s](size_t from, const char* word) -> size_t
    {
        for (size_t i = from; i < s.size(); i++)
        {
            size_t p = i;
            if ((i == 0 || isspace(static_cast<unsigned char>(s[i - 1]))) && TakeWord(s, p, word))
                return i;
        }
        return std::string::npos;
    };
    const size_t toAt = until(pos, "to");
    const size_t doAt = until(pos, "do");
    if (toAt == std::string::npos || doAt == std::string::npos || doAt < toAt)
        return false;
    const std::string from = s.substr(pos, toAt - pos);
    size_t stepAt = until(toAt, "step");
    if (stepAt != std::string::npos && stepAt > doAt)
        stepAt = std::string::npos;
    size_t p = toAt + 2;
    const std::string to = s.substr(p, (stepAt != std::string::npos ? stepAt : doAt) - p);
    std::string stepValue = "1";
    if (stepAt != std::string::npos)
        stepValue = s.substr(stepAt + 4, doAt - stepAt - 4);
    pos = doAt + 2;
    if (!TakeBracketed(s, pos, '{', '}', out.body) || !AtEnd(s, pos))
        return false;
    out.init = var + " = (" + from + ")";
    out.condition = "if ((" + stepValue + ") >= 0) then {" + var + " <= (" + to + ")} else {" + var + " >= (" + to + ")}";
    out.step = var + " = " + var + " + (" + stepValue + ")";
    return true;
}
} // namespace

Script* Script::FindByHandle(int handle)
{
    const auto found = ScriptHandles().find(handle);
    return found == ScriptHandles().end() ? nullptr : found->second.GetLink();
}

Script* Script::CreateFromCode(RString name, RString code, GameValuePar argument)
{
    Script* script = new Script();
    script->_argument = argument;
    script->_maxLinesAtOnce = 100;
    script->_name = name;
    script->_handle = NextScriptHandle();
    ScriptHandles()[script->_handle] = script;

    static RString empty("");
    const auto addLine = [script](RString waitUntil, RString suspendUntil, RString statement)
    {
        const int i = script->_lines.Add();
        script->_lines[i].waitUntil = waitUntil;
        script->_lines[i].suspendUntil = suspendUntil;
        script->_lines[i].condition = empty;
        script->_lines[i].statement = statement;
    };

    // Scheduled code may suspend anywhere: a sleep/waitUntil inside while, if or for is
    // compiled into script lines with labels and gotos (the same machinery SQS uses), so
    // the loop yields to the frame instead of spinning unscheduled.
    int labelCount = 0;
    const auto newLabel = [&labelCount]() { return "__a1l" + std::to_string(labelCount++); };
    const auto placeLabel = [script](const std::string& name)
    {
        const int i = script->_labels.Add();
        script->_labels[i].name = RString(name.c_str());
        script->_labels[i].line = script->_lines.Size();
    };
    const auto addCondLine = [script](const std::string& condition, const std::string& statement)
    {
        const int i = script->_lines.Add();
        script->_lines[i].condition = RString(condition.c_str());
        script->_lines[i].statement = RString(statement.c_str());
    };
    const auto addGoto = [&addCondLine](const std::string& label) { addCondLine("", "goto \"" + label + "\""); };

    std::vector<std::string> loopExits; // where exitWith leaves to (innermost loop)
    std::vector<std::pair<std::string, std::string>> forEachVars; // (list, index) of compiled forEach loops
    const std::string scriptEnd = newLabel();
    std::function<void(const std::string&)> emit;
    const auto emitBlock = [&emit](const std::string& code)
    {
        for (const std::string& statement : SplitSqfStatements(code))
            emit(statement);
    };
    emit = [&](const std::string& statement)
    {
        std::string rest;
        std::string body;
        SqfControl control;
        if (StartsWithWord(statement, "waitUntil", rest) && UnwrapBraces(rest, body))
        {
            // A multi-statement condition evaluates as code; its value is the last statement's.
            const std::vector<std::string> parts = SplitSqfStatements(body);
            const std::string condition = parts.size() <= 1 ? body : "call {" + body + "}";
            addLine(RString(condition.c_str()), empty, empty);
        }
        else if (StartsWithWord(statement, "sleep", rest))
        {
            const std::string wait = "__waituntil = _time+(" + rest + ")";
            addLine(empty, empty, RString(wait.c_str()));
            addLine(empty, RString("__waituntil"), empty);
        }
        else if (ParseSqfControl(statement, control) && control.kind == SqfControl::ExitWith)
        {
            // exitWith leaves the innermost compiled scope (loop body, if/switch branch);
            // at the top level of the script it ends the script, as in Arma.
            const std::string skip = newLabel();
            addCondLine("!(" + control.condition + ")", "goto \"" + skip + "\"");
            emitBlock(control.body);
            addGoto(loopExits.empty() ? scriptEnd : loopExits.back());
            placeLabel(skip);
        }
        else if (HasSuspension(statement) && ParseSqfControl(statement, control) &&
                 control.kind != SqfControl::ExitWith)
        {
            const std::string top = newLabel();
            const std::string next = newLabel();
            switch (control.kind)
            {
                case SqfControl::While:
                    placeLabel(top);
                    addCondLine("!(" + control.condition + ")", "goto \"" + next + "\"");
                    loopExits.push_back(next);
                    emitBlock(control.body);
                    loopExits.pop_back();
                    addGoto(top);
                    placeLabel(next);
                    break;
                case SqfControl::ExitWith:
                    break;
                case SqfControl::ForEach:
                {
                    const std::string id = std::to_string(labelCount);
                    const std::string list = "_a1felist" + id;
                    const std::string index = "_a1feindex" + id;
                    addCondLine("", list + " = (" + control.condition + "); " + index + " = 0");
                    placeLabel(top);
                    addCondLine("!(" + index + " < count " + list + ")", "goto \"" + next + "\"");
                    addCondLine("", "_x = " + list + " select " + index);
                    loopExits.push_back(next);
                    forEachVars.emplace_back(list, index);
                    emitBlock(control.body);
                    forEachVars.pop_back();
                    loopExits.pop_back();
                    addCondLine("", index + " = " + index + " + 1");
                    addGoto(top);
                    placeLabel(next);
                    // an enclosing compiled forEach gets its own _x back
                    if (!forEachVars.empty())
                        addCondLine("", "if (" + forEachVars.back().second + " < count " + forEachVars.back().first +
                                            ") then {_x = " + forEachVars.back().first + " select " +
                                            forEachVars.back().second + "}");
                    break;
                }
                case SqfControl::If:
                {
                    const std::string otherwise = newLabel();
                    addCondLine("!(" + control.condition + ")", "goto \"" + otherwise + "\"");
                    loopExits.push_back(next);
                    emitBlock(control.body);
                    addGoto(next);
                    placeLabel(otherwise);
                    emitBlock(control.elseBody);
                    loopExits.pop_back();
                    placeLabel(next);
                    break;
                }
                case SqfControl::Switch:
                {
                    const std::string value = "_a1switch" + std::to_string(labelCount);
                    addCondLine("", value + " = (" + control.condition + ")");
                    const SqfCase* fallback = nullptr;
                    for (const SqfCase& c : control.cases)
                    {
                        if (c.values.empty())
                        {
                            fallback = &c;
                            continue;
                        }
                        std::string match;
                        for (const std::string& v : c.values)
                            match += (match.empty() ? "" : " || ") + std::string("(") + value + " == (" + v + "))";
                        const std::string skip = newLabel();
                        addCondLine("!(" + match + ")", "goto \"" + skip + "\"");
                        emitBlock(c.code);
                        addGoto(next);
                        placeLabel(skip);
                    }
                    if (fallback)
                        emitBlock(fallback->code);
                    placeLabel(next);
                    break;
                }
                case SqfControl::For:
                    addCondLine("", control.init);
                    placeLabel(top);
                    addCondLine("!(" + control.condition + ")", "goto \"" + next + "\"");
                    loopExits.push_back(next);
                    emitBlock(control.body);
                    loopExits.pop_back();
                    addCondLine("", control.step);
                    addGoto(top);
                    placeLabel(next);
                    break;
            }
        }
        else
        {
            addLine(empty, empty, RString(statement.c_str()));
        }
    };
    emitBlock(StripSqfComments(code.Data()));
    placeLabel(scriptEnd);
    return script;
}

} // namespace Poseidon

GameValue ScriptSpawn(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    using namespace Poseidon;
    Script* script = Script::CreateFromCode("spawn", oper2, oper1);
    GWorld->AddScript(script);
    return GameValue(static_cast<float>(script->GetHandle()));
}

bool PreprocessScriptFile(const char* name, std::string& out); // GameStateExtWorldConfig.cpp

GameValue ScriptExecVM(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    using namespace Poseidon;
    RString name = oper2;
    QIFStreamB in;
    std::string code;
    // Arma 2 scripts rely on #include / #define: run them through the preprocessor
    // preprocessFile uses. Arma 1 content keeps the plain read below.
    if (Arma1Compat::Generation() == 2 && PreprocessScriptFile(name, code))
    {
        // preprocessed; '#line' markers and the like are dropped below
    }
    else if (OpenScript(in, name))
    {
        while (true)
        {
            const int c = in.get();
            if (in.eof() || in.fail() || c < 0)
                break;
            code.push_back(static_cast<char>(c));
        }
    }
    else
    {
        LOG_WARN(Script, "execVM: script '{}' not found", (const char*)name);
    }
    // Preprocessor directives are not expanded here; report them instead of guessing.
    if (Arma1Compat::Generation() != 2 &&
        (code.find("#include") != std::string::npos || code.find("#define") != std::string::npos))
        LOG_WARN(Script, "execVM '{}': preprocessor directives are not supported and were ignored",
                 (const char*)name);
    std::string filtered;
    size_t pos = 0;
    while (pos < code.size())
    {
        size_t end = code.find('\n', pos);
        if (end == std::string::npos)
            end = code.size();
        std::string line = code.substr(pos, end - pos);
        size_t b = 0;
        while (b < line.size() && isspace(static_cast<unsigned char>(line[b])))
            b++;
        if (b >= line.size() || line[b] != '#')
            filtered += line + "\n";
        pos = end + 1;
    }
    Script* script = Script::CreateFromCode(name, RString(filtered.c_str()), oper1);
    GWorld->AddScript(script);
    return GameValue(static_cast<float>(script->GetHandle()));
}

GameValue ScriptTerminate(const GameState* state, GameValuePar oper1)
{
    using namespace Poseidon;
    if (Script* script = Script::FindByHandle(static_cast<int>(static_cast<float>(oper1))))
        script->Exit();
    return GameValue();
}

GameValue ScriptIsDone(const GameState* state, GameValuePar oper1)
{
    using namespace Poseidon;
    Script* script = Script::FindByHandle(static_cast<int>(static_cast<float>(oper1)));
    return GameValue(!script || script->IsTerminated());
}

GameValue ScriptWaitUntilUnscheduled(const GameState* state, GameValuePar oper1)
{
    using namespace Poseidon;
    static bool reported = false;
    if (!reported)
    {
        reported = true;
        LOG_WARN(Script, "waitUntil outside the top level of spawned code cannot suspend; evaluated once: {}",
                 (const char*)RString(oper1));
    }
    return GameValue(GWorld->GetGameState()->EvaluateBool(RString(oper1)));
}

GameValue ScriptSleepUnscheduled(const GameState* state, GameValuePar oper1)
{
    static bool reported = false;
    if (!reported)
    {
        reported = true;
        LOG_WARN(Script, "sleep outside the top level of spawned code cannot suspend; ignored");
    }
    return GameValue();
}

GameValue PreloadCameraReady(const GameState* state, GameValuePar oper1)
{
    // Streaming loads the scene around the camera on demand; nothing to wait for.
    return GameValue(true);
}

GameValue PreloadObjectReady(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return GameValue(true);
}

GameValue CampaignKeyActive(const GameState* state, GameValuePar oper1)
{
    return GameValue(Poseidon::Arma1Compat::IsKeyActive((const char*)RString(oper1)));
}

GameValue CampaignKeyActivate(const GameState* state, GameValuePar oper1)
{
    Poseidon::Arma1Compat::ActivateKey((const char*)RString(oper1));
    return GameValue();
}

GameValue ScriptExecVMUnary(const GameState* state, GameValuePar oper1)
{
    return ScriptExecVM(state, GameValue(), oper1);
}
