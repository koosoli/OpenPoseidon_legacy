
#include <Evaluator/express.hpp>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <Poseidon/Foundation/Strings/Bstring.hpp>
#include <stdio.h>
#include <string.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/platform.hpp>

#include <Random/randomGen.hpp>

#ifndef CHECK
#define CHECK(command)         \
    {                          \
        LSError err = command; \
        if (err != 0)          \
            return err;        \
    }
#endif

// Meyers singleton accessors - no global constructors
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"

static ArchiveFunctions& GArchiveFunctions()
{
    static ArchiveFunctions instance;
    return instance;
}

static GameStateInfoFunctions& GGameStateInfoFunctions()
{
    static GameStateInfoFunctions instance;
    return instance;
}

#pragma clang diagnostic pop

// Static member initializers.
//
// These are constant-initialized to nullptr (no dynamic initializer →
// guaranteed zero at program load, before any global constructor runs).
// The real default ("no-op for unregistered callers") is provided
// lazily by GArchiveFunctions() / GGameStateInfoFunctions() — Meyer's
// singletons that materialize on first use.  This avoids the static
// initialization order fiasco with the registration constructors in
// engine/Poseidon/Game/Scripting/ExpressExt.cpp
// (GParamArchiveFunctions / GGameStateStringtableInfoFunctions): if
// ExpressExt's constructors ran first, an unconditional initializer
// here would clobber the real implementation back to the no-op,
// silently dropping every save/load of a GameState._vars entry.
ArchiveFunctions* GameState::_defaultArchiveFunctions = nullptr;
GameStateInfoFunctions* GameState::_defaultInfoFunctions = nullptr;

DEFINE_FAST_ALLOCATOR(GameDataNil)
DEFINE_FAST_ALLOCATOR(GameDataScalar)
DEFINE_FAST_ALLOCATOR(GameDataBool)
DEFINE_FAST_ALLOCATOR(GameDataNothing)
DEFINE_FAST_ALLOCATOR(GameDataString)
DEFINE_FAST_ALLOCATOR(GameDataArray)
DEFINE_FAST_ALLOCATOR(GameEvaluator)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
static GameValue& GameValueNil()
{
    static GameValue instance(new GameDataNil(GameVoid));
    return instance;
}
#pragma clang diagnostic pop

#define MAX_EXPR_LEN 1024

#define lenof(arr) (sizeof(arr) / sizeof(*arr))

#ifdef _WIN32
#define M_PI 3.1415926536
#endif
#define NOTHING GameValue()

void GameState::ShowDebug(const RString& content, int timeMs) const
{
    DefaultInfoFunctions()->DisplayDebugMessage(content, timeMs);
}

void GameState::SetError(EvalError error, ...) const
{
    _e->_error = error;
    if (error == EvalOK)
    {
        _e->_errorText = RString();
        return;
    }
#ifndef ACCESS_ONLY
    const char* errorText = DefaultInfoFunctions()->GetErrorString(error);
    BString<MAX_EXPR_LEN> formated;

    va_list va;
    va_start(va, error);
    vsprintf((char*)(const char*)formated, errorText, va);
    va_end(va);

    _e->_errorText = formated.cstr();
    _e->_errorCarretPos = _e->_pos - _e->_pos0;

#endif
}

void GameState::TypeError(GameType exp, GameType was, const char* name) const
{
    char expName[512];
    char wasName[512];
    snprintf(expName, sizeof(expName), "%s", (const char*)GetTypeName(exp));
    snprintf(wasName, sizeof(wasName), "%s", (const char*)GetTypeName(was));
    SetError(EvalType, wasName, expName);
    if (name)
    {
        // append name before error message
        RString withName = name + RString(": ") + _e->_errorText;
        _e->_errorText = withName;
    }
}

void GameState::vynech() const
{
    for (;;)
    {
        while (isspace(*_e->_pos))
        {
            _e->_pos++;
        }
        // Arma 2 accepts C++ comments in code that never went through the preprocessor
        // (init lines, triggers, waypoint statements).
        if (s_singleQuoteStrings && _e->_pos[0] == '/' && _e->_pos[1] == '/')
        {
            while (*_e->_pos && *_e->_pos != '\n')
                _e->_pos++;
            continue;
        }
        if (s_singleQuoteStrings && _e->_pos[0] == '/' && _e->_pos[1] == '*')
        {
            const char* end = strstr(_e->_pos + 2, "*/");
            _e->_pos = end ? end + 2 : _e->_pos + strlen(_e->_pos);
            continue;
        }
        return;
    }
}

Real GameState::sejmid() const
{
    char* end;
    Real cisl = strtod(_e->_pos, &end);
    if (end == _e->_pos)
    {
        SetError(EvalNum);
        return 0;
    }
    _e->_pos = end;
    return cisl;
}

inline bool isalphaext(char c)
{
    return isalpha(c) || c == '_';
}

inline bool isalnumext(char c)
{
    return isalnum(c) || c == '_';
}

#define OPEN_STRING '{'
#define CLOSE_STRING '}'

GameValue GameState::Const() const
{
    vynech();
    if (isalphaext(_e->_pos[0])) // variable
    {
        char name[MAX_EXPR_LEN];
        char* wName = name;
        int i = 0;
        while (isalnumext(_e->_pos[0]))
        {
            if (i < MAX_EXPR_LEN - 1)
            {
                *wName++ = *_e->_pos, i++;
            }
            _e->_pos++;
        }
        *wName = 0;
        strlwr(name);
        if (*name == '_')
        {
            GameVarSpace* space = _e->local;
            if (!space)
            {
                SetError(EvalNamespace);
                return GameValue();
            }
            GameValue value;
            space->VarGet(name, value);
            return value;
        }
        if (_e->_checkOnly || IsNull(_vars[name]))
        {
            const GameNular& o = _nulars[name];
            if (_nulars.NotNull(o))
            {
                if (_e->_checkOnly)
                {
                    return GameValue(new GameDataNil(o._retType));
                }
                else
                {
                    return (*o._proc)(this);
                }
            }
        }

        if (!VarGoodName(name))
        {
            SetError(EvalBadVar);
            return GameValue();
        }
        if (_e->_checkOnly)
        {
            return CreateGameValue(GameVoid);
        }
        return VarGet(name);
    }
    else if (*_e->_pos == '"' || (*_e->_pos == '\'' && s_singleQuoteStrings))
    {
        // string ("..." always; '...' for Arma 2 content, see s_singleQuoteStrings)
        const char quote = *_e->_pos;
        const char* start = ++_e->_pos;
        RString string = "";
        for (;;)
        {
            while (*_e->_pos != quote)
            {
                if (!*_e->_pos)
                {
                    SetError(EvalNum);
                    return RString();
                }
                _e->_pos++;
            }
            // string from start to _e->_pos
            string = string + RString(start, _e->_pos - start);
            // check if another quote mark is encountered
            // if yes, append it, and continue parsing, otherwise we are done
            ++_e->_pos;
            if (*_e->_pos == quote)
            {
                string = string + RString(quote == '"' ? "\"" : "'");
                ++_e->_pos;
                start = _e->_pos;
            }
            else
            {
                break;
            }
        }

        return string;
    }
    else if (_e->_pos[0] == OPEN_STRING)
    {
        // special case of string value
        const char* start = ++_e->_pos;
        bool closed = false;
        const char* end = start;
        int level = 1;
        for (;;)
        {
            // note: any quotes inside may prevent closing bracket
            if (*end == '"' || (*end == '\'' && s_singleQuoteStrings))
            {
                const char quote = *end;
                end++;
                // skip anything until another quote mark
                while (*end != 0 && *end != quote)
                {
                    end++;
                }
                if (*end != quote)
                {
                    SetError(EvalCloseB);
                    return RString();
                }
            }
            else if (*end == OPEN_STRING)
            {
                level++;
            }
            else if (*end == CLOSE_STRING)
            {
                if (--level == 0)
                {
                    closed = true;
                    break;
                }
            }
            else if (*end == 0)
            {
                break;
            }
            end++;
        }
        if (closed)
        {
            _e->_pos = end + 1;
            return RString(start, end - start);
        }
        else
        {
            SetError(EvalCloseB);
            return RString();
        }
    }
    else if (_e->_pos[0] == '0' && _e->_pos[1] == 'x')
    {
        bool Fg = false;
        // unsigned accumulator: an over-long hex literal overflows int (UB); the
        // unsigned wraparound reproduces the original two's-complement result.
        unsigned vrt = 0;
        _e->_pos += 2;
        while (isxdigit(*_e->_pos))
        {
            Fg = true;
            vrt *= 16u;
            if (isdigit(*_e->_pos))
            {
                vrt += *_e->_pos - '0';
            }
            else
            {
                vrt += toupper(*_e->_pos) - ('A' - 10);
            }
            _e->_pos++;
        }
        if (!Fg)
        {
            SetError(EvalNum);
        }
        return float((int)vrt);
    }
    else if (*_e->_pos == '$') /* hex */
    {
        bool Fg = false;
        // unsigned accumulator: an over-long hex literal overflows int (UB); the
        // unsigned wraparound reproduces the original two's-complement result.
        unsigned vrt = 0;
        _e->_pos++;
        while (isxdigit(*_e->_pos))
        {
            Fg = true;
            vrt *= 16u;
            if (isdigit(*_e->_pos))
            {
                vrt += *_e->_pos - '0';
            }
            else
            {
                vrt += toupper(*_e->_pos) - ('A' - 10);
            }
            _e->_pos++;
        }
        if (!Fg)
        {
            SetError(EvalNum);
        }
        return float((int)vrt);
    }
    else
    {
        return sejmid();
    }
}

static GameValue Soucet(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return (float)oper1 + (float)oper2;
}
static GameValue Rozdil(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return (float)oper1 - (float)oper2;
}
static GameValue Soucin(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return (float)oper1 * (float)oper2;
}
static GameValue Podil(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if ((float)oper2 == 0)
    {
        state->SetError(EvalDivZero);
        return 0.0f;
    }
    return (float)oper1 / (float)oper2;
}
static GameValue Zbytek(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if ((float)oper2 == 0)
    {
        state->SetError(EvalDivZero);
        return 0.0f;
    }
    return (float)fmod((float)oper1, (float)oper2);
}
static GameValue Na(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return (float)pow((float)oper1, (float)oper2);
}

static GameValue CmpE(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return (float)oper1 == (float)oper2;
}
static GameValue CmpL(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return (float)oper1 < (float)oper2;
}
static GameValue CmpG(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return (float)oper1 > (float)oper2;
}
static GameValue CmpLE(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return (float)oper1 <= (float)oper2;
}
static GameValue CmpGE(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return (float)oper1 >= (float)oper2;
}
static GameValue CmpNE(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return (float)oper1 != (float)oper2;
}

static GameValue StrCmpNE(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return strcmpi((GameStringType)oper1, (GameStringType)oper2) != 0;
}

static GameValue StrCmpE(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return strcmpi((GameStringType)oper1, (GameStringType)oper2) == 0;
}

[[maybe_unused]] static GameValue VoidCmpNE(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return !oper1.IsEqualTo(oper2);
}

[[maybe_unused]] static GameValue VoidCmpE(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return oper1.IsEqualTo(oper2);
}

static GameValue StrAdd(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return (GameStringType)oper1 + (GameStringType)oper2;
}

static GameValue ListAdd(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    // make return object to avoid unneccessary copy
    GameValue retValue(new GameDataArray);
    GameArrayType& ret = retValue;

    const GameArrayType& add1 = oper1;
    const GameArrayType& add2 = oper2;
    ret.Realloc(add1.Size() + add2.Size());

    for (int i = 0; i < add1.Size(); i++)
    {
        ret.Add(add1[i]);
    }
    for (int i = 0; i < add2.Size(); i++)
    {
        ret.Add(add2[i]);
    }
    return retValue;
}

static GameValue ListSub(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    // make return object to avoid unneccessary copy
    GameValue retValue(new GameDataArray);
    GameArrayType& ret = retValue;

    const GameArrayType& op1 = oper1;
    const GameArrayType& op2 = oper2;
    ret.Realloc(op1.Size());

    for (int i = 0; i < op1.Size(); i++)
    {
        // check if it is member of op2
        bool isInOp2 = false;
        for (int j = 0; j < op2.Size(); j++)
        {
            if (op1[i].IsEqualTo(op2[j]))
            {
                isInOp2 = true;
                break;
            }
        }
        if (isInOp2)
        {
            continue;
        }
        // if not, add it to returned list
        ret.Add(op1[i]);
    }
    return retValue;
}

static GameValue BoolAnd(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return bool(oper1) && bool(oper2);
}
static GameValue BoolOr(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return bool(oper1) || bool(oper2);
}

static GameValue Plus(const GameState* state, GameValuePar oper1)
{
    // make identical copy of argument
    return GameValue(oper1.GetData()->Clone());
}
static GameValue Minus(const GameState* state, GameValuePar oper1)
{
    return -(float)oper1;
}
static GameValue Stupne(const GameState* state, GameValuePar oper1)
{
    return (float)(2 * M_PI * (float)oper1 / 360);
}
static GameValue Radian(const GameState* state, GameValuePar oper1)
{
    return (float)((float)oper1 * 360 / (2 * M_PI));
}

static GameValue Random(const GameState* state, GameValuePar oper1)
{
    float value = GRandGen.RandomValue();
    return (float)(value * (float)oper1);
}

static GameValue Sinus(const GameState* state, GameValuePar oper1)
{
    return (float)sin((double)Stupne(state, oper1));
}
static GameValue Cosinus(const GameState* state, GameValuePar oper1)
{
    return (float)cos((double)Stupne(state, (float)oper1));
}
static GameValue Tangens(const GameState* state, GameValuePar oper1)
{
    return (float)tan((double)Stupne(state, (float)oper1));
}
static GameValue ASinus(const GameState* state, GameValuePar oper1)
{
    return Radian(state, (float)asin((float)oper1));
}
static GameValue ACosinus(const GameState* state, GameValuePar oper1)
{
    return Radian(state, (float)acos((float)oper1));
}
static GameValue ATangens(const GameState* state, GameValuePar oper1)
{
    return Radian(state, (float)atan((float)oper1));
}
static GameValue Atan2(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if ((float)oper1 == 0 && (float)oper2 == 0)
    {
        state->SetError(EvalDivZero);
        return 0.0f;
    }
    return Radian(state, (float)atan2((float)oper1, (float)oper2));
}
static GameValue FLog(const GameState* state, GameValuePar oper1)
{
    return (float)log10((float)oper1);
}
static GameValue FLn(const GameState* state, GameValuePar oper1)
{
    return (float)log((float)oper1);
}
static GameValue Exp(const GameState* state, GameValuePar oper1)
{
    return (float)exp((float)oper1);
}
static GameValue Odmocnina(const GameState* state, GameValuePar oper1)
{
    return (float)sqrt((float)oper1);
}

static GameValue Abs(const GameState* state, GameValuePar oper1)
{
    return (float)fabs((float)oper1);
}

// Arma 1 additions (round half away from zero, as Arma does)
static GameValue Round(const GameState* state, GameValuePar oper1)
{
    return (float)std::round((float)oper1);
}

static GameValue Floor(const GameState* state, GameValuePar oper1)
{
    return (float)std::floor((float)oper1);
}

static GameValue Ceil(const GameState* state, GameValuePar oper1)
{
    return (float)std::ceil((float)oper1);
}

static GameValue BoolNot(const GameState* state, GameValuePar oper1)
{
    return !bool(oper1);
}

static GameValue VoidNil(const GameState* state)
{
    return GameValue(new GameDataNil(GameVoid));
}

static GameValue BoolTrue(const GameState* state)
{
    return true;
}
static GameValue BoolFalse(const GameState* state)
{
    return false;
}

static GameValue ScalarPi(const GameState* state)
{
    return float(M_PI);
}

static GameValue ListCount(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    return (float)array.Size();
}

static GameValue ListSelect(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper1;
    float index = oper2;
    int sel = toInt(index);
    if (sel < 0 || sel >= array.Size())
    {
        if (sel != array.Size())
        {
            state->SetError(EvalDivZero);
        }
        return GameValue();
    }
    return array[sel];
}

static GameValue ListResize(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (oper1.GetReadOnly())
    {
        state->SetError(EvalBadVar);
        return NOTHING;
    }
    const GameArrayType& array = oper1;
    float index = oper2;
    int sel = toInt(index);
    GameArrayType& arrayNC = const_cast<GameArrayType&>(array);
    arrayNC.Resize(sel);
    return NOTHING;
}

static bool CheckArrayInValue(const GameArrayType& array1, const GameValue& value)
{
    if (value.GetType() != GameArray)
    {
        return false;
    }
    const GameArrayType& array2 = value;
    if (&array1 == &array2)
    {
        return true;
    }
    for (int i = 0; i < array2.Size(); i++)
    {
        if (CheckArrayInValue(array1, array2[i]))
        {
            return true;
        }
    }
    return false;
}

static GameValue ListSet(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper1;
    const GameArrayType& arg = oper2;
    if (arg.Size() != 2)
    {
        state->SetError(EvalDim, array.Size(), 2);
        return NOTHING;
    }
    if (arg[0].GetType() != GameScalar)
    {
        state->TypeError(GameScalar, arg[0].GetType());
        return NOTHING;
    }

    float index = arg[0];
    const GameValue& value = arg[1];
    int sel = toInt(index);
    if (sel < 0)
    {
        state->SetError(EvalDivZero);
        return NOTHING;
    }

    if (oper1.GetReadOnly())
    {
        state->SetError(EvalBadVar);
        return NOTHING;
    }
    GameArrayType& arrayNC = const_cast<GameArrayType&>(array);
    arrayNC.Access(sel); // grows the array if needed
    // storing an array that contains this array would create a recursive
    // structure, which later crashes; reject it instead
    if (CheckArrayInValue(array, value))
    {
        Fail("Recursive array");
        state->SetError(EvalDim);
        return NOTHING;
    }

    arrayNC[sel] = value;

    return NOTHING;
}

static GameValue StringCall(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    state->VarSetLocal("_this", oper1, true);
    RString expression = oper2;
    GameValue ret = state->EvaluateMultiple(expression);
    state->EndContext();
    return ret;
}

static GameValue StringLocal(const GameState* state, GameValuePar oper1)
{
    if (oper1.GetType() == GameString)
    {
        RString varName = oper1;
        if (varName[0] != '_')
        {
            state->SetError(EvalNamespace);
            return NOTHING;
        }
        const_cast<GameState*>(state)->VarLocal(varName);
    }
    else
    {
        const GameArrayType& array = oper1;
        for (int i = 0; i < array.Size(); i++)
        {
            if (array[i].GetType() != GameString)
            {
                state->TypeError(GameString, array[i].GetType());
                break;
            }
            RString varName = array[i];
            if (varName[0] != '_')
            {
                state->SetError(EvalNamespace);
                return NOTHING;
            }
            const_cast<GameState*>(state)->VarLocal(varName);
        }
    }
    return NOTHING;
}

static GameValue StringCallNoArg(const GameState* state, GameValuePar oper1)
{
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    RString expression = oper1;
    GameValue ret = state->EvaluateMultiple(expression);
    state->EndContext();
    return ret;
}

static GameValue StringIgnore(const GameState* state, GameValuePar oper1)
{
    return NOTHING;
}

// Thread-local storage for the value passed to exitWith { expr }.
// Set by StringExitWith when the condition is true; read and cleared
// by loop handlers (ForDoOp, StringRepeat) to propagate the exit value.
static thread_local GameValue tls_exitWithValue;

static GameValue StringRepeat(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    int maxIters = 10000;
    for (int i = 0;; i++)
    {
        if (i >= maxIters)
        {
            LOG_WARN(Core, "SQF: max iteration count exceeded in while loop");
            state->SetError(EvalGen);
            break;
        }
        if (GameState::s_abortLoops)
            break;
        // test condition
        {
            GameVarSpace local(state->GetContext());
            state->BeginContext(&local);
            RString expression = oper1;

            GameValue value = state->EvaluateMultiple(expression);
            bool test = false;
            if (value.GetType() & GameBool)
            {
                test = bool(value);
            }
            else
            {
                state->TypeError(GameBool, value.GetType());
            }

            state->EndContext();
            if (!test)
            {
                break;
            }
        }
        // perform loop body
        {
            GameVarSpace local(state->GetContext());
            state->BeginContext(&local);
            RString expression = oper2;
            const_cast<GameState*>(state)->Execute(expression);
            state->EndContext();
        }
        if (state->GetLastError() == EvalBreak)
        {
            GameValue exitVal = tls_exitWithValue;
            const_cast<GameState*>(state)->SetError(EvalOK);
            return exitVal;
        }
    }
    return NOTHING;
}

static GameValue StringElse(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameValue arrayVal = state->CreateGameValue(GameArray);
    GameArrayType& array = arrayVal;
    array.Realloc(2);
    array.Append() = oper1;
    array.Append() = oper2;
    return arrayVal;
}

static GameValue IfBool(const GameState* state, GameValuePar oper1)
{
    bool value = oper1;
    return GameValue(new GameDataIf(value));
}

static GameValue WhileString(const GameState* state, GameValuePar oper1)
{
    GameStringType value = oper1;
    return GameValue(new GameDataWhile(value));
}

static GameValue StringThen(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    bool test = oper1;
    if (test)
    {
        GameVarSpace local(state->GetContext());
        state->BeginContext(&local);
        RString expression = oper2;
        GameValue ret = state->EvaluateMultiple(expression);
        state->EndContext();
        return ret;
    }
    return NOTHING;
}

// if (cond) exitWith { code } — evaluates code when cond is true, stores the
// value in tls_exitWithValue, and signals EvalBreak to stop further statement
// execution in the current scope.  Loop handlers (ForDoOp, StringRepeat) catch
// EvalBreak, clear it, and use tls_exitWithValue as the loop's return value.
// At top level (no enclosing loop), EvaluateMultiple captures the value when
// it sees EvalBreak so the tri runner still receives "FAIL:..." strings.
static GameValue StringExitWith(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    bool test = oper1;
    if (test)
    {
        GameVarSpace local(state->GetContext());
        state->BeginContext(&local);
        RString expression = oper2;
        GameValue ret = state->EvaluateMultiple(expression);
        state->EndContext();
        tls_exitWithValue = ret;
        const_cast<GameState*>(state)->SetError(EvalBreak);
        return ret;
    }
    return NOTHING;
}

// --- for / from / to / step / do (for-loop variant) -------------------------

static GameValue ForVarString(const GameState*, GameValuePar oper1)
{
    RString var = oper1;
    return GameValue(new GameDataFor(var));
}

static GameValue ForFromOp(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    auto* forData = static_cast<const GameDataFor*>(oper1.GetData());
    float from = oper2;
    return GameValue(new GameDataForFrom(forData->GetString(), from));
}

static GameValue ForToOp(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    auto* fromData = static_cast<const GameDataForFrom*>(oper1.GetData());
    float to = oper2;
    return GameValue(new GameDataForTo(fromData->GetVar(), fromData->GetFrom(), to));
}

static GameValue ForStepOp(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    auto* toData = static_cast<const GameDataForTo*>(oper1.GetData());
    float step = oper2;
    return GameValue(new GameDataForTo(toData->GetVar(), toData->GetFrom(), toData->GetTo(), step));
}

static GameValue ForDoOp(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    auto* toData = static_cast<const GameDataForTo*>(oper1.GetData());
    float from = toData->GetFrom();
    float to = toData->GetTo();
    float step = toData->GetStep();
    RString body = oper2;

    if (step == 0.0f)
    {
        state->SetError(EvalGen);
        return NOTHING;
    }

    GameValue exitVal = NOTHING;
    int maxIters = 100000;
    int count = 0;
    for (float i = from; (step > 0 ? i <= to : i >= to); i += step)
    {
        if (count++ >= maxIters)
        {
            LOG_WARN(Core, "SQF: max iteration count exceeded in for loop");
            state->SetError(EvalGen);
            break;
        }
        if (GameState::s_abortLoops)
            break;
        GameVarSpace local(state->GetContext());
        state->BeginContext(&local);
        state->VarSetLocal(toData->GetVar(), GameValue(i));
        const_cast<GameState*>(state)->Execute(body);
        state->EndContext();
        if (state->GetLastError() == EvalBreak)
        {
            exitVal = tls_exitWithValue;
            const_cast<GameState*>(state)->SetError(EvalOK);
            break;
        }
        if (state->GetLastError() != EvalOK)
            break;
    }
    return exitVal;
}

static GameValue ArrayThen(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    bool test = oper1;
    const GameArrayType& array = oper2;

    if (array.Size() != 2)
    {
        state->SetError(EvalDim, array.Size(), 2);
        return NOTHING;
    }
    if (array[0].GetType() != GameString)
    {
        state->TypeError(GameScalar, array[0].GetType());
        return NOTHING;
    }
    if (array[1].GetType() != GameString)
    {
        state->TypeError(GameScalar, array[1].GetType());
        return NOTHING;
    }

    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);

    RString expression = test ? array[0] : array[1];
    GameValue ret = state->EvaluateMultiple(expression);
    state->EndContext();
    return ret;
}

[[maybe_unused]] static GameValue StringEval(const GameState* state, GameValuePar oper1)
{
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    RString expression = oper1;
    GameValue ret = state->Evaluate(expression);
    state->EndContext();
    return ret;
}

// parseSimpleArray string — evaluate a string representation of a SQF value
// (typically an array literal like "[1,2,3]") and return it as a GameValue.
static GameValue ParseSimpleArray(const GameState* state, GameValuePar oper1)
{
    RString expression = oper1;
    return state->Evaluate(expression);
}

static GameValue ListCountCond(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameVarSpace local(state->GetContext());
    int count = 0;
    state->BeginContext(&local);
    RString expression = oper1;
    const GameArrayType& array = oper2;
    for (int i = 0; i < array.Size(); i++)
    {
        const GameValue& val = array[i];
        // set local varible - will be deleted on EndContext
        state->VarSetLocal("_x", val, true);
        if (state->EvaluateBool(expression))
        {
            count++;
        }
        if (state->GetLastError())
        {
            state->EndContext();
            return GameValue();
        }
    }
    state->EndContext();
    return (float)count;
}

static GameValue ListForEach(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    RString expression = oper1;
    const GameArrayType& array = oper2;
    for (int i = 0; i < array.Size(); i++)
    {
        const GameValue& val = array[i];
        // set local varible - will be deleted on EndContext
        state->VarSetLocal("_x", val, true);
        const_cast<GameState*>(state)->Execute(expression);
        if (state->GetLastError())
        {
            state->EndContext();
            return GameValue();
        }
    }
    state->EndContext();
    return NOTHING;
}

static GameValue ListIsIn(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper2;
    for (int i = 0; i < array.Size(); i++)
    {
        const GameValue& val = array[i];
        if (val.IsEqualTo(oper1))
        {
            return true;
        }
    }
    return false;
}

static GameValue ListFind(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper1;
    for (int i = 0; i < array.Size(); i++)
    {
        const GameValue& val = array[i];
        if (val.IsEqualTo(oper2))
        {
            return (float)i;
        }
    }
    return -1.0f;
}

// Meyers singleton accessor for default binary operators - no global constructor
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
static const GameOperator* GetDefaultBinary(int* outSize = nullptr)
{
    static const GameOperator DefaultBinary[] = {
        GameOperator(GameScalar, "^", mocnina, Na, GameScalar, GameScalar),
        GameOperator(GameScalar, "*", soucin, Soucin, GameScalar, GameScalar),
        GameOperator(GameScalar, "/", soucin, Podil, GameScalar, GameScalar),
        GameOperator(GameScalar, "%", soucin, Zbytek, GameScalar, GameScalar),
        GameOperator(GameScalar, "mod", soucin, Zbytek, GameScalar, GameScalar),
        GameOperator(GameScalar, "atan2", soucin, Atan2, GameScalar, GameScalar),
        GameOperator(GameScalar, "+", soucet, Soucet, GameScalar, GameScalar),
        GameOperator(GameScalar, "-", soucet, Rozdil, GameScalar, GameScalar),
        GameOperator(GameBool, ">=", comparison, CmpGE, GameScalar, GameScalar),
        GameOperator(GameBool, "<=", comparison, CmpLE, GameScalar, GameScalar),
        GameOperator(GameBool, ">", comparison, CmpG, GameScalar, GameScalar),
        GameOperator(GameBool, "<", comparison, CmpL, GameScalar, GameScalar),
        GameOperator(GameBool, "==", comparison, CmpE, GameScalar, GameScalar),
        GameOperator(GameBool, "!=", comparison, CmpNE, GameScalar, GameScalar),

        GameOperator(GameBool, "==", comparison, StrCmpE, GameString, GameString),
        GameOperator(GameBool, "!=", comparison, StrCmpNE, GameString, GameString),

        GameOperator(GameString, "+", soucet, StrAdd, GameString, GameString),
        GameOperator(GameArray, "+", soucet, ListAdd, GameArray, GameArray),
        GameOperator(GameArray, "-", soucet, ListSub, GameArray, GameArray),

        GameOperator(GameBool, "&&", logicAnd, BoolAnd, GameBool, GameBool),
        GameOperator(GameBool, "and", logicAnd, BoolAnd, GameBool, GameBool),
        GameOperator(GameBool, "||", logicOr, BoolOr, GameBool, GameBool),
        GameOperator(GameBool, "or", logicOr, BoolOr, GameBool, GameBool),

        GameOperator(GameVoid, "select", function, ListSelect, GameArray, GameScalar),
        GameOperator(GameVoid, "select", function, ListSelect, GameArray, GameBool),
        GameOperator(GameNothing, "set", function, ListSet, GameArray, GameArray),
        GameOperator(GameNothing, "resize", function, ListResize, GameArray, GameScalar),
        GameOperator(GameScalar, "count", function, ListCountCond, GameString, GameArray),
        GameOperator(GameNothing, "forEach", function, ListForEach, GameString, GameArray),
        GameOperator(GameBool, "in", function, ListIsIn, GameVoid, GameArray),
        GameOperator(GameScalar, "find", function, ListFind, GameArray, GameVoid),

        GameOperator(GameAny, "then", function, StringThen, GameIf, GameString),
        GameOperator(GameAny, "then", function, ArrayThen, GameIf, GameArray),
        GameOperator(GameAny, "exitWith", function, StringExitWith, GameIf, GameString),
        GameOperator(GameArray, "else", functionFirst, StringElse, GameString, GameString),
        GameOperator(GameNothing, "do", function, StringRepeat, GameWhile, GameString),

        // for "var" from N to M [step S] do { body }
        GameOperator(GameForFrom, "from", function, ForFromOp, GameFor, GameScalar),
        GameOperator(GameForTo, "to", function, ForToOp, GameForFrom, GameScalar),
        GameOperator(GameForTo, "step", function, ForStepOp, GameForTo, GameScalar),
        GameOperator(GameAny, "do", function, ForDoOp, GameForTo, GameString),

        GameOperator(GameAny, "call", function, StringCall, GameVoid, GameString),
    };
    if (outSize)
    {
        *outSize = lenof(DefaultBinary);
    }
    return DefaultBinary;
}
#pragma clang diagnostic pop

// Meyers singleton accessor for default unary functions - no global constructor
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
static const GameFunction* GetDefaultUnary(int* outSize = nullptr)
{
    static const GameFunction DefaultUnary[] = {
        GameFunction(GameScalar, "sin", Sinus, GameScalar),
        GameFunction(GameScalar, "random", Random, GameScalar),
        GameFunction(GameScalar, "cos", Cosinus, GameScalar),
        GameFunction(GameScalar, "tg", Tangens, GameScalar),
        GameFunction(GameScalar, "tan", Tangens, GameScalar),
        GameFunction(GameScalar, "asin", ASinus, GameScalar),
        GameFunction(GameScalar, "acos", ACosinus, GameScalar),
        GameFunction(GameScalar, "atg", ATangens, GameScalar),
        GameFunction(GameScalar, "atan", ATangens, GameScalar),
        GameFunction(GameScalar, "deg", Radian, GameScalar),
        GameFunction(GameScalar, "rad", Stupne, GameScalar),
        GameFunction(GameScalar, "log", FLog, GameScalar),
        GameFunction(GameScalar, "ln", FLn, GameScalar),
        GameFunction(GameScalar, "exp", Exp, GameScalar),
        GameFunction(GameScalar, "sqrt", Odmocnina, GameScalar),
        GameFunction(GameScalar, "abs", Abs, GameScalar),
        GameFunction(GameScalar, "round", Round, GameScalar),
        GameFunction(GameScalar, "floor", Floor, GameScalar),
        GameFunction(GameScalar, "ceil", Ceil, GameScalar),
        GameFunction(GameScalar, "+", Plus, GameScalar), // known special cases
        GameFunction(GameArray, "+", Plus, GameArray),   // replication
        GameFunction(GameScalar, "-", Minus, GameScalar),
        GameFunction(GameBool, "!", BoolNot, GameBool),
        GameFunction(GameBool, "not", BoolNot, GameBool),

        GameFunction(GameScalar, "count", ListCount, GameArray),
        GameFunction(GameAny, "call", StringCallNoArg, GameString),
        GameFunction(GameNothing, "comment", StringIgnore, GameString),
        GameFunction(GameNothing, "private", StringLocal, GameString | GameArray),
        GameFunction(GameAny, "parseSimpleArray", ParseSimpleArray, GameString),

        GameFunction(GameIf, "if", IfBool, GameBool),
        GameFunction(GameWhile, "while", WhileString, GameString),
        GameFunction(GameFor, "for", ForVarString, GameString),
    };
    if (outSize)
    {
        *outSize = lenof(DefaultUnary);
    }
    return DefaultUnary;
}
#pragma clang diagnostic pop

// Meyers singleton accessor for default nular operators - no global constructor
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
static const GameNular* GetDefaultNular(int* outSize = nullptr)
{
    static const GameNular DefaultNular[] = {GameNular(GameVoid, "nil", VoidNil), GameNular(GameBool, "true", BoolTrue),
                                             GameNular(GameBool, "false", BoolFalse),
                                             GameNular(GameScalar, "pi", ScalarPi)};
    if (outSize)
    {
        *outSize = lenof(DefaultNular);
    }
    return DefaultNular;
}
#pragma clang diagnostic pop

// Meyers singleton accessors for constant strings - no global constructors
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
static const RString& GetBinOp()
{
    static const RString BinOp = "<bin>";
    return BinOp;
}

static const RString& GetLstOp()
{
    static const RString LstOp = "<lst>";
    return LstOp;
}
#pragma clang diagnostic pop

void GameState::TypeErrorOperator(const char* name, GameType left, GameType right) const
{
    // check which types are accepted as operands
    GameType leftE = GameType(0);
    GameType rightE = GameType(0);
    const GameOperators& operators = _operators[name];
    if (!_operators.IsNull(operators))
    {
        for (int i = 0; i < operators.Size(); i++)
        {
            leftE |= operators[i]._argType1;
            rightE |= operators[i]._argType2;
        }
    }
    if ((leftE & left) != left)
    {
        TypeError(leftE, left, name);
    }
    else if ((rightE & right) != right)
    {
        TypeError(rightE, right, name);
    }
    else
    {
        LOG_WARN(Core, "SQF operator {}: no overload for operand types 0x{:x}, 0x{:x}", name, (unsigned)left, (unsigned)right);
        SetError(EvalGen);
    }
}

void GameState::TypeErrorFunction(const char* name, GameType right) const
{
    // check which types are accepted as operands
    GameType rightE = GameType(0);
    const GameFunctions& functions = _functions[name];
    if (!_functions.IsNull(functions))
    {
        for (int i = 0; i < functions.Size(); i++)
        {
            rightE |= functions[i]._argType;
        }
    }
    if ((rightE & right) != right)
    {
        TypeError(rightE, right, name);
    }
    else
    {
        LOG_WARN(Core, "SQF function {}: no overload for operand type 0x{:x}", name, (unsigned)right);
        SetError(EvalGen);
    }
}

// A user-defined variable hides a function or operator of the same name, so a
// script keeps working even when a new command later collides with one of its
// variable names.
void GameState::VyhCast(int Prio) const
{
    while (_e->SP > 0)
    {
        RString oper1 = _e->_operStack[_e->SP - 1];
        if (oper1 == GetBinOp())
        {
            if (_e->SP <= 1)
            {
                return;
            }
            RString oper2 = _e->_operStack[_e->SP - 2];
            if (oper2 == GetBinOp() || oper2 == GetLstOp())
            {
                return;
            }
            if (Prio > _e->_priorStack[_e->SP - 2])
            {
                return;
            }
            // binary
            PoseidonAssert(_e->UB[_e->SP - 1] == -1);
            if (_e->UB[_e->SP - 2] == 2)
            {
                // find function with appropriate name

                if (!_e->_checkOnly)
                {
                    const GameOperator* op = nullptr;

                    // The parser already decided this is an operator (a variable of the
                    // same name -- an object called "spawn" -- cannot be meant here).
                    {
                        const GameOperators& operators = _operators[oper2];
                        if (!_operators.IsNull(operators))
                        {
                            for (int i = 0; i < operators.Size(); i++)
                            {
                                const GameOperator& o = operators[i];
                                if (!(o._argType1 & _e->_stack[_e->SP - 2].GetType()))
                                {
                                    continue;
                                }
                                if (!(o._argType2 & _e->_stack[_e->SP - 1].GetType()))
                                {
                                    continue;
                                }
                                op = &o;
                                break;
                            }
                        }
                    }

                    if (!op)
                    {
                        TypeErrorOperator(oper2, _e->_stack[_e->SP - 2].GetType(), _e->_stack[_e->SP - 1].GetType());
                        return;
                    }

                    if (_e->_stack[_e->SP - 2].GetNil() || _e->_stack[_e->SP - 1].GetNil())
                    {
                        _e->_stack[_e->SP - 2] = GameValue(new GameDataNil(op->_retType));
                    }
                    else
                    {
                        _e->_stack[_e->SP - 2] = (*op->_proc)(this, _e->_stack[_e->SP - 2], _e->_stack[_e->SP - 1]);
                    }
                }
                else
                {
                    GameType possible(0);
                    const GameOperators& operators = _operators[oper2];
                    if (!_operators.IsNull(operators))
                    {
                        for (int i = 0; i < operators.Size(); i++)
                        {
                            const GameOperator& o = operators[i];
                            if (!(o._argType1 & _e->_stack[_e->SP - 2].GetType()))
                            {
                                continue;
                            }
                            if (!(o._argType2 & _e->_stack[_e->SP - 1].GetType()))
                            {
                                continue;
                            }
                            possible |= o._retType;
                        }
                    }

                    if (possible == 0)
                    {
                        TypeErrorOperator(oper2, _e->_stack[_e->SP - 2].GetType(), _e->_stack[_e->SP - 1].GetType());
                        return;
                    }

                    _e->_stack[_e->SP - 2] = GameValue(new GameDataNil(possible));
                }
            }
            else
            {
                PoseidonAssert(_e->UB[_e->SP - 2] == 1);
                if (!_e->_checkOnly)
                {
                    const GameFunction* op = nullptr;

                    if (IsNull(_vars[oper2]))
                    {
                        const GameFunctions& functions = _functions[oper2];
                        if (!_functions.IsNull(functions))
                        {
                            for (int i = 0; i < functions.Size(); i++)
                            {
                                const GameFunction& f = functions[i];
                                if (f._argType & _e->_stack[_e->SP - 1].GetType())
                                {
                                    op = &f;
                                    break;
                                }
                            }
                        }
                    }

                    if (!op)
                    {
                        TypeErrorFunction(oper2, _e->_stack[_e->SP - 1].GetType());
                        return;
                    }

                    if (_e->_stack[_e->SP - 1].GetNil())
                    {
                        _e->_stack[_e->SP - 2] = GameValue(new GameDataNil(op->_retType));
                    }
                    else
                    {
                        _e->_stack[_e->SP - 2] = (*op->_proc)(this, _e->_stack[_e->SP - 1]);
                    }
                }
                else
                {
                    GameType possible(0);
                    const GameFunctions& functions = _functions[oper2];
                    if (!_functions.IsNull(functions))
                    {
                        for (int i = 0; i < functions.Size(); i++)
                        {
                            const GameFunction& f = functions[i];
                            if (f._argType & _e->_stack[_e->SP - 1].GetType())
                            {
                                possible |= f._retType;
                            }
                        }
                    }

                    if (possible == 0)
                    {
                        TypeErrorFunction(oper2, _e->_stack[_e->SP - 1].GetType());
                        return;
                    }
                    _e->_stack[_e->SP - 2] = GameValue(new GameDataNil(possible));
                }
            }
            _e->_operStack[_e->SP - 2] = GetBinOp();
            _e->_priorStack[_e->SP - 2] = _e->_parPrior + _e->_listPrior;
            _e->UB[_e->SP - 2] = -1;
            // remove argument
            _e->_stack[_e->SP - 1] = GameValue();
            _e->SP--;
        }
        else
        {
            PoseidonAssert(oper1 == GetLstOp());
            // list
            return;
        }
    }
    if (_e->_error)
    {
        return;
    }
}

inline int CMP(const char* Ps, const char* Ps1)
{
    return strnicmp(Ps, Ps1, strlen(Ps1));
}

inline int CMPN(const char* Ps, const char* Ps1, int n)
{
    return strnicmp(Ps, Ps1, n);
}

void GameState::CleanStack() const
{
    // remove all values from the stack
    while (_e->SP > 0)
    {
        _e->_stack[--_e->SP] = GameValue();
    }
}

// RAII guard that snapshots the parser-private fields of the shared
// EvalState on Vyhod entry and restores them on exit.  Vyhod is not
// inherently re-entrant — it resets _e->SP, _e->_pos, etc. — but the
// engine routinely re-enters it from C++ operator handlers (per-frame
// condition evals on 'camCommitted _camera', cheat bindings, animation
// rotation matrices, UI menu condition strings, AI arcades, detector
// triggers, mission scripts).  Without the snapshot a nested call
// overwrites the outer parser stack, and the stale off-array Refs left
// behind crash in Ref::Release.  Snapshotting at this level — below the
// public entry points — makes every caller of Vyhod reentrancy-safe.
// Stack snapshot is ~16 KB; restore is one memcpy each on exit.
struct VyhodFrameGuard
{
    GameEvaluator& e;
    // Parser-private fields that get reset by Vyhod entry and that
    // outer callers depend on for their own parse continuation.
    // Notably NOT included: _error / _errorText / _errorCarretPos —
    // those are public output of the eval (read by GetLastError /
    // CheckEvaluate / ShowError) and need to propagate from this call
    // back to the caller, exactly like before.  The reentrancy bug
    // was a parser-stack-state corruption, not an error-channel one.
    int savedSP;
    int savedParPrior;
    int savedListPrior;
    const char* savedPos;
    const char* savedPos0;
    const char* savedSubexpBeg;
    const char* savedSubexpEnd;
    // Operand-stack slots up to outer's SP only.  Slots beyond outer's
    // SP are scratch space the next caller is welcome to use.  Saving
    // just the live prefix keeps the cost proportional to actual
    // nesting depth (typical < 10, capped at SL=256).
    std::vector<GameValue> savedStack;
    std::vector<RString> savedOperStack;
    std::vector<int> savedUB;
    std::vector<int> savedPriorStack;

    explicit VyhodFrameGuard(GameEvaluator& ev)
        : e(ev), savedSP(ev.SP), savedParPrior(ev._parPrior), savedListPrior(ev._listPrior), savedPos(ev._pos),
          savedPos0(ev._pos0), savedSubexpBeg(ev._subexpBeg), savedSubexpEnd(ev._subexpEnd)
    {
        if (savedSP > 0)
        {
            savedStack.assign(e._stack.begin(), e._stack.begin() + savedSP);
            savedOperStack.assign(e._operStack.begin(), e._operStack.begin() + savedSP);
            savedUB.assign(e.UB.begin(), e.UB.begin() + savedSP);
            savedPriorStack.assign(e._priorStack.begin(), e._priorStack.begin() + savedSP);
        }
    }

    ~VyhodFrameGuard()
    {
        for (int i = 0; i < savedSP; ++i)
        {
            e._stack[i] = savedStack[i];
            e._operStack[i] = savedOperStack[i];
            e.UB[i] = savedUB[i];
            e._priorStack[i] = savedPriorStack[i];
        }
        e.SP = savedSP;
        e._parPrior = savedParPrior;
        e._listPrior = savedListPrior;
        e._pos = savedPos;
        e._pos0 = savedPos0;
        e._subexpBeg = savedSubexpBeg;
        e._subexpEnd = savedSubexpEnd;
    }
};

GameValue GameState::Vyhod() const
{
    const GameFunctions* functions;
    const GameOperators* operators;
    _e->_parPrior = 0;
    _e->_listPrior = 0;
    _e->SP = 0;
    _e->_error = EvalOK;
    _e->_errorText = RString();
    if (*_e->_pos == ';' || *_e->_pos == 0)
    {
        // detect empty expression
        return NOTHING;
    }
    for (;;)
    {
        vynech();
        if (*_e->_pos == '(')
        {
            _e->_pos++;
            _e->_parPrior += zavorky;
            continue;
        }
        if (*_e->_pos == '[')
        {
            // evaluate list - no recursion?
            _e->_pos++;
            vynech();
            if (*_e->_pos == ']')
            {
                // empty list construction
                _e->_pos++;

                if (!_e->Reserve(_e->SP))
                {
                    SetError(EvalLineLong);
                    return NOTHING;
                }
                _e->_stack[_e->SP] = GameArrayType();
                _e->_priorStack[_e->SP] = -1;
                _e->UB[_e->SP] = -1;
                _e->_operStack[_e->SP] = GetBinOp();
                _e->SP++;

                goto CekejBinar;
            }
            else
            {
                _e->_listPrior += zavorky;
            }
            continue;
        }
        // check unary operator -
        functions = nullptr;
        if (isalpha(*_e->_pos))
        {
            // scan identifier
            int idlen = 0;
            while (isalnumext(_e->_pos[idlen]))
            {
                idlen++;
            }

            RString name(_e->_pos, idlen);
            name.Lower();

            if (_e->_checkOnly || IsNull(_vars[name]))
            {
                functions = &_functions[name];
            }
        }
        else
        {
            RString name2(_e->_pos, 2);
            functions = &_functions[name2];
            if (_functions.IsNull(*functions))
            {
                RString name1(_e->_pos, 1);
                functions = &_functions[name1];
            }
        }
        if (!functions || _functions.IsNull(*functions))
        {
            if (!_e->Reserve(_e->SP))
            {
                SetError(EvalLineLong);
                return NOTHING;
            }
            // value on stack top
            _e->_stack[_e->SP] = Const();
            _e->_priorStack[_e->SP] = -1;
            _e->UB[_e->SP] = -1;
            _e->_operStack[_e->SP] = GetBinOp();
            _e->SP++;

            if (_e->_error)
            {
                return NOTHING;
            }
        CekejBinar:
            vynech();
            switch (*_e->_pos)
            {
                case ',':
                {
                    if (_e->_listPrior <= 0)
                    {
                        if (_e->_checkOnly)
                        {
                            SetError(EvalOpenB);
                            return 0.0f;
                        }
                        goto Semicolon;
                    }
                    _e->_pos++;

                    VyhCast(_e->_parPrior + _e->_listPrior);

                    if (_e->_error)
                    {
                        return NOTHING;
                    }
                    // leave value on stack and continue with evaluation
                    _e->_operStack[_e->SP - 1] = GetLstOp(); // no op
                    _e->UB[_e->SP - 1] = 3;                  // list element
                    _e->_priorStack[_e->SP - 1] = _e->_parPrior + _e->_listPrior;

                    break;
                }
                case ']':
                {
                    _e->_pos++;

                    VyhCast(_e->_parPrior + _e->_listPrior);
                    if (_e->_error)
                    {
                        return NOTHING;
                    }

                    // change value to list value
                    _e->_operStack[_e->SP - 1] = GetLstOp(); // no op
                    _e->UB[_e->SP - 1] = 3;                  // list element
                    _e->_priorStack[_e->SP - 1] = _e->_parPrior + _e->_listPrior;
                    // leave value on stack and continue with evaluation

                    _e->_listPrior -= zavorky;
                    if (_e->_listPrior < 0)
                    {
                        SetError(EvalOpenB);
                        return 0.0f;
                    }
                    // pop all arguments with prior>=_listPrior
                    GameValue arrayVal = CreateGameValue(GameArray);
                    GameArrayType& array = arrayVal;

                    int SPP = _e->SP;
                    while (SPP > 0 && _e->_priorStack[SPP - 1] >= _e->_listPrior + _e->_parPrior + zavorky)
                    {
                        PoseidonAssert(_e->_operStack[SPP - 1] == GetLstOp());
                        PoseidonAssert(_e->UB[SPP - 1] == 3);
                        SPP--;
                    }
                    array.Realloc(_e->SP - SPP);
                    for (int i = SPP; i < _e->SP; i++)
                    {
                        array.Add(_e->_stack[i]);
                    }
                    _e->SP = SPP;
                    if (!_e->Reserve(_e->SP))
                    {
                        SetError(EvalLineLong);
                        return NOTHING;
                    }
                    // push array value
                    _e->_stack[_e->SP] = arrayVal;
                    _e->_priorStack[_e->SP] = -1;
                    _e->UB[_e->SP] = -1;
                    _e->_operStack[_e->SP] = GetBinOp();
                    _e->SP++;
                    goto CekejBinar;
                }
                case ')':
                    _e->_pos++;
                    _e->_parPrior -= zavorky;
                    if (_e->_parPrior < 0)
                    {
                        SetError(EvalOpenB);
                        return 0.0f;
                    }
                    goto CekejBinar;
                case ';':
                Semicolon:
                {
                    if (_e->_parPrior != 0)
                    {
                        SetError(EvalCloseB);
                        return 0.0f;
                    }
                    VyhCast(0);
                    if (_e->_error)
                    {
                        return NOTHING;
                    }

                    PoseidonAssert(_e->SP == 1);
                    PoseidonAssert(_e->UB[_e->SP - 1] == -1);
                    PoseidonAssert(_e->_operStack[_e->SP - 1] == GetBinOp());
                    GameValue value = _e->_stack[--_e->SP];
                    _e->_stack[_e->SP] = GameValue();
                    return value;
                }
                case 0:
                {
                    if (_e->_parPrior != 0)
                    {
                        SetError(EvalCloseB);
                        return 0.0f;
                    }
                    if (_e->_listPrior != 0)
                    {
                        SetError(EvalCloseB);
                        return 0.0f;
                    }
                    VyhCast(0);
                    if (_e->_error)
                    {
                        return NOTHING;
                    }

                    PoseidonAssert(_e->SP == 1);
                    GameValue value = _e->_stack[--_e->SP];
                    _e->_stack[_e->SP] = GameValue();
                    return value;
                }
                default:
                {
                    // if first letter is alphaext, scan whole identifier
                    int idlen = 0;
                    operators = nullptr;
                    if (isalphaext(_e->_pos[0]))
                    {
                        while (isalnumext(_e->_pos[idlen]))
                        {
                            idlen++;
                        }

                        RString name(_e->_pos, idlen);
                        name.Lower();
                        // Here an operator is expected, so a variable of the same name
                        // cannot be meant: Arma 1 missions name objects "spawn" and
                        // still write `[] spawn {...}`. Only when no operator of that
                        // name exists is the name left unresolved, as before.
                        operators = &_operators[name];
                        if (!_e->_checkOnly && !IsNull(_vars[name]) && _operators.IsNull(*operators))
                        {
                            operators = nullptr;
                        }
                    }
                    else
                    {
                        RString name2(_e->_pos, 2);
                        operators = &_operators[name2];
                        if (_operators.IsNull(*operators))
                        {
                            RString name1(_e->_pos, 1);
                            operators = &_operators[name1];
                        }
                    }

                    if (!operators || _operators.IsNull(*operators))
                    {
                        char name[256];
                        // idlen is the unbounded identifier length; clamp the copy
                        // into this fixed error-message buffer (truncation is fine).
                        int n = idlen < (int)sizeof(name) - 1 ? idlen : (int)sizeof(name) - 1;
                        strncpy(name, _e->_pos, n);
                        name[n] = 0;
                        SetError(EvalOper, name);
                        return NOTHING;
                    }
                    _e->_pos += strlen(operators->_name);
                    int prior = operators->_priority + _e->_parPrior + _e->_listPrior;
                    VyhCast(prior);
                    if (_e->_error)
                    {
                        return NOTHING;
                    }
                    _e->_operStack[_e->SP - 1] = operators->_name;
                    _e->UB[_e->SP - 1] = 2;
                    _e->_priorStack[_e->SP - 1] = prior;
                    break;
                }
            } // switch
        } // binary or special
        else // unary operator
        {
            _e->_pos += strlen(functions->_name);
            if (!_e->Reserve(_e->SP))
            {
                SetError(EvalLineLong);
                return NOTHING;
            }
            _e->_stack[_e->SP] = 0.0f; // no first argument
            _e->_operStack[_e->SP] = functions->_name;
            _e->UB[_e->SP] = 1;
            _e->_priorStack[_e->SP] = unar + _e->_parPrior + _e->_listPrior;
            _e->SP++;
            // save unary operator
        }
    } // while
    /*NOTREACHED*/
}

GameValue::GameValue()
{
    _data = nullptr;
}

GameValue::GameValue(GameData* data)
{
    _data = data;
}

#if !ACCESS_ONLY
GameData* GameData::CreateObject(ParamArchive& ar)
{
    bool nil = false;
    if (GameState::Serialize(ar, "nil", nil, 1, false) != 0)
    {
        return nullptr;
    }
    GameType type = GameVoid;
    if (GameState::Serialize(ar, "type", type, 1) != 0)
    {
        return nullptr;
    }
    if (nil)
    {
        return new GameDataNil(type);
    }

    return GameState::CreateGameData(ar, type);
}

LSError GameData::Serialize(ParamArchive& ar)
{
    if (GameState::IsSaving(ar))
    {
        GameType type = GetType();
        CHECK(GameState::Serialize(ar, "type", type, 1));
    }
    return (LSError)0;
}
#endif

RString GameDataNil::GetText() const
{
    if (_type == GameVoid)
    {
        return "scalar bool array string 0xfcffffef";
    }
    if (_type == ~0)
    {
        return "all";
    }
    char buf[MAX_EXPR_LEN];
    strcpy(buf, "");
    int rest = _type;
    while (rest)
    {
        if (*buf)
        {
            strcat(buf, " ");
        }
        if (rest & GameScalar)
        {
            strcat(buf, "scalar"), rest &= ~GameScalar;
        }
        else if (rest & GameBool)
        {
            strcat(buf, "bool"), rest &= ~GameBool;
        }
        else if (rest & GameArray)
        {
            strcat(buf, "array"), rest &= ~GameArray;
        }
        else if (rest & GameString)
        {
            strcat(buf, "string"), rest &= ~GameString;
        }
        else if (rest & GameNothing)
        {
            strcat(buf, "nothing"), rest &= ~GameNothing;
        }
        else
        {
            sprintf(buf + strlen(buf), "0x%x", _type);
            break;
        }
    }
    return buf;
}

bool GameDataNothing::IsEqualTo(const GameData* data) const
{
    return false; // nil is never equal to anything
}

bool GameDataNil::IsEqualTo(const GameData* data) const
{
    return false; // nil is never equal to anything
}

#ifndef ACCESS_ONLY
LSError GameDataNil::Serialize(ParamArchive& ar)
{
    bool nil = true;
    CHECK(GameState::Serialize(ar, "nil", nil, 1));
    // nothing but type is serialized
    CHECK(GameState::Serialize(ar, "type", _type, 1));
    return (LSError)0;
}
#endif

RString GameDataScalar::GetText() const
{
    BString<MAX_EXPR_LEN> buf;
    sprintf(buf, "%g", _value);
    return (const char*)buf;
}

bool GameDataScalar::IsEqualTo(const GameData* data) const
{
    float val1 = GetScalar();
    float val2 = data->GetScalar();
    return val1 == val2;
}

#ifndef ACCESS_ONLY
LSError GameDataScalar::Serialize(ParamArchive& ar)
{
    CHECK(base::Serialize(ar));
    CHECK(GameState::Serialize(ar, "value", _value, 1));
    return (LSError)0;
}
#endif

RString GameDataBool::GetText() const
{
    return _value ? "true" : "false";
}

bool GameDataBool::IsEqualTo(const GameData* data) const
{
    bool val1 = GetBool();
    bool val2 = data->GetBool();
    return val1 == val2;
}

#ifndef ACCESS_ONLY
LSError GameDataBool::Serialize(ParamArchive& ar)
{
    CHECK(base::Serialize(ar));
    CHECK(GameState::Serialize(ar, "value", _value, 1));
    return (LSError)0;
}
#endif

RString GameDataString::GetText() const
{
    return RString("\"") + _value + RString("\"");
}

bool GameDataString::IsEqualTo(const GameData* data) const
{
    RString val1 = GetString();
    RString val2 = data->GetString();
    return val1 == val2;
}

#ifndef ACCESS_ONLY
LSError GameDataString::Serialize(ParamArchive& ar)
{
    CHECK(base::Serialize(ar));
    CHECK(GameState::Serialize(ar, "value", _value, 1));
    return (LSError)0;
}
#endif

GameDataArray::GameDataArray() : _readOnly(false) {}

GameDataArray::GameDataArray(const GameArrayType& value) : _value(value), _readOnly(false) {}

GameDataArray::~GameDataArray() = default;

GameData* GameDataArray::Clone() const
{
    GameDataArray* clone = new GameDataArray(*this);
    // clone is writable even when the source is read-only
    clone->SetReadOnly(false);
    return clone;
}

RString GameDataArray::GetText() const
{
    RString val("[");
    for (int i = 0; i < _value.Size(); i++)
    {
        if (i > 0)
        {
            val = val + RString(",");
        }
        const GameValue& value = _value[i];
        if (value.GetType() == GameArray)
        {
            const GameArrayType& array = value;
            if (&array == &_value)
            {
                // prevent simple recursion
                // note: same situation may happen via an intermediate array
                // but this is almost impossible to catch
                val = val + RString("...");
                continue;
            }
        }
        val = val + _value[i].GetText();
    }
    val = val + RString("]");
    return val;
}

bool GameDataArray::IsEqualTo(const GameData* data) const
{
    const GameArrayType& val1 = GetArray();
    const GameArrayType& val2 = data->GetArray();
    if (val1.Size() != val2.Size())
    {
        return false;
    }
    return false;
}

#ifndef ACCESS_ONLY
LSError GameDataArray::Serialize(ParamArchive& ar)
{
    CHECK(base::Serialize(ar));
    CHECK(GameState::Serialize(ar, "value", _value, 1));
    return (LSError)0;
}
#endif

GameValue::GameValue(const GameValue& value)
{
    _data = value._data;
}

void GameValue::operator=(const GameValue& value)
{
    _data = value._data;
}

void GameValue::Duplicate(const GameValue& value)
{
    if (!value._data)
    {
        _data = nullptr;
        return;
    }
    _data = value._data->Clone();
    if (!_data)
    {
        LOG_ERROR(Core, "Data not cloned: {}", (const char*)value.GetDebugText());
        _data = new GameDataNil(GameAny);
    }
}

#ifndef ACCESS_ONLY
LSError GameValue::Serialize(ParamArchive& ar)
{
    // during serialization _data must point to valid object
    if (!_data)
    {
        _data = new GameDataNothing;
    }
    CHECK(GameState::Serialize(ar, "data", _data, 1));
    if (_data && _data->GetType() == GameNothing)
    {
        _data = nullptr;
    }
    return (LSError)0;
}
#endif

RString GameValue::GetText() const
{
    if (!_data)
    {
        return "<null>";
    }
    return _data->GetText();
}

RString GameValue::GetDebugText() const
{
    if (!_data)
    {
        return "<null>";
    }
    RString type = _data->GetTypeName();
    RString text = _data->GetText();
    return type + RString(" ") + text;
}

bool GameValue::IsEqualTo(const GameValue& value) const
{
    // check type
    if (GetNil() || value.GetNil())
    {
        return false;
    }
    if (!GetData() || !value.GetData())
    {
        return false;
    }
    if (value.GetType() != GetType())
    {
        return false;
    }
    // if it is same type, call corresponding method
    return GetData()->IsEqualTo(value.GetData());
}

void GameState::NewType(const char* name, GameType val, CreateGameDataFunction* func, RString tname)
{
    GameTypeType en;
    en.name = name;
    en.value = int(val);
    en.createFunction = func;
    en.typeName = tname;
    _typeNames.Add(en);
}

void GameState::NewNularOps(const GameNular* f, int count)
{
    for (int i = 0; i < count; i++)
    {
        GGameState.NewNularOp(f[i]);
    }
}
void GameState::NewFunctions(const GameFunction* f, int count)
{
    for (int i = 0; i < count; i++)
    {
        GGameState.NewFunction(f[i]);
    }
}
void GameState::NewOperators(const GameOperator* f, int count)
{
    for (int i = 0; i < count; i++)
    {
        GGameState.NewOperator(f[i]);
    }
}

void GameState::NewNularOp(const GameNular& f)
{
    const GameNular& o = f;

    GameNular& nt = _nulars[f._name];
    if (_nulars.IsNull(nt))
    {
        _nulars.Add(o);
    }
}
void GameState::NewFunction(const GameFunction& f)
{
    GameFunctions& ft = _functions[f._name];
    if (_functions.IsNull(ft))
    {
        _functions.Add(GameFunctions(f._opName));
    }
    GameFunctions& functions = _functions[f._name];
    GameFunction o = f;
    o._name = functions._name;

    int n = functions.Size();
    functions.Realloc(n + 1);
    functions.Resize(n + 1);
    functions[n] = o;
}
void GameState::NewOperator(const GameOperator& op)
{
    GameOperators& ot = _operators[op._name];
    if (_operators.IsNull(ot))
    {
        _operators.Add(GameOperators(op._opName, op._priority));
    }
    GameOperators& operators = _operators[op._name];
    GameOperator o = op;
    o._name = operators._name;

    int n = operators.Size();
    operators.Realloc(n + 1);
    operators.Resize(n + 1);
    operators[n] = o;
}

struct DicContext
{
    AutoArray<RStringS>& _dic;
    bool (*_filter)(const char* word);
    DicContext(AutoArray<RStringS>& dic, bool (*filter)(const char* word)) : _dic(dic), _filter(filter) {}
};

inline void AddDicFunc(RString name, DicContext& context)
{
    if (context._filter(name))
    {
        context._dic.Add(name);
    }
}

static void AddDicFuncNul(const GameNular& op, const GameNularsType* list, void* context)
{
    AddDicFunc(op._opName, *(DicContext*)context);
}
static void AddDicFuncUn(const GameFunctions& op, const GameFunctionsType* list, void* context)
{
    AddDicFunc(op._opName, *(DicContext*)context);
}
static void AddDicFuncBin(const GameOperators& op, const GameOperatorsType* list, void* context)
{
    AddDicFunc(op._opName, *(DicContext*)context);
}

void GameState::AppendNularOpList(AutoArray<RStringS>& dic, bool (*filter)(const char* word)) const
{
    DicContext context(dic, filter);
    _nulars.ForEach(AddDicFuncNul, &context);
}
void GameState::AppendFunctionList(AutoArray<RStringS>& dic, bool (*filter)(const char* word)) const
{
    DicContext context(dic, filter);
    _functions.ForEach(AddDicFuncUn, &context);
}
void GameState::AppendOperatorList(AutoArray<RStringS>& dic, bool (*filter)(const char* word)) const
{
    DicContext context(dic, filter);
    _operators.ForEach(AddDicFuncBin, &context);
}

GameState::GameState()
{
    Reset();
    _actContext = -1;
    BeginContext(nullptr);
}

GameState::~GameState()
{
    _vars.Clear();
    _typeNames.Clear();
    _functions.Clear();
    _operators.Clear();
    _nulars.Clear();
}

GameData* CreateGameDataScalar()
{
    return new GameDataScalar();
}
GameData* CreateGameDataBool()
{
    return new GameDataBool();
}
GameData* CreateGameDataArray()
{
    return new GameDataArray();
}
GameData* CreateGameDataString()
{
    return new GameDataString();
}
GameData* CreateGameDataNothing()
{
    return new GameDataNothing();
}
GameData* CreateGameDataIf()
{
    return new GameDataIf();
}
GameData* CreateGameDataWhile()
{
    return new GameDataWhile();
}
GameData* CreateGameDataFor()
{
    return new GameDataFor();
}
GameData* CreateGameDataForFrom()
{
    return new GameDataForFrom();
}
GameData* CreateGameDataForTo()
{
    return new GameDataForTo();
}

void GameState::Init()
{
    _typeNames.Clear();
    NewType("SCALAR", GameScalar, CreateGameDataScalar, DefaultInfoFunctions()->GetTypeName(GameScalar));
    NewType("BOOL", GameBool, CreateGameDataBool, DefaultInfoFunctions()->GetTypeName(GameBool));
    NewType("ARRAY", GameArray, CreateGameDataArray, DefaultInfoFunctions()->GetTypeName(GameArray));
    NewType("STRING", GameString, CreateGameDataString, DefaultInfoFunctions()->GetTypeName(GameString));
    NewType("NOTHING", GameNothing, CreateGameDataNothing, DefaultInfoFunctions()->GetTypeName(GameNothing));

    NewType("IF", GameIf, CreateGameDataIf, DefaultInfoFunctions()->GetTypeName(GameIf));
    NewType("WHILE", GameWhile, CreateGameDataWhile, DefaultInfoFunctions()->GetTypeName(GameWhile));
    NewType("FOR", GameFor, CreateGameDataFor, DefaultInfoFunctions()->GetTypeName(GameFor));
    NewType("FOR_FROM", GameForFrom, CreateGameDataForFrom, DefaultInfoFunctions()->GetTypeName(GameForFrom));
    NewType("FOR_TO", GameForTo, CreateGameDataForTo, DefaultInfoFunctions()->GetTypeName(GameForTo));

    _functions.Clear();
    int unarySize = 0;
    const GameFunction* unaryArray = GetDefaultUnary(&unarySize);
    for (int i = 0; i < unarySize; i++)
    {
        NewFunction(unaryArray[i]);
    }
    _operators.Clear();
    int binarySize = 0;
    const GameOperator* binaryArray = GetDefaultBinary(&binarySize);
    for (int i = 0; i < binarySize; i++)
    {
        NewOperator(binaryArray[i]);
    }
    _nulars.Clear();
    int nularSize = 0;
    const GameNular* nularArray = GetDefaultNular(&nularSize);
    for (int i = 0; i < nularSize; i++)
    {
        NewNularOp(nularArray[i]);
    }
}

void GameState::Reset()
{
    _vars.Clear();
    // reserved internal variables
    VarSet("this", false, true);
}

// Arma 1/2: a local variable used where no local scope exists (a unit's init line,
// "_x = [this] execVM ...") lives in one shared scratch scope instead of being an error.
static GameVarSpace& GlobalScratchLocals()
{
    static GameVarSpace scratch;
    return scratch;
}

GameValue GameState::VarGet(const char* name) const
{
    // convert name to lower case
    RString source = name;
    source.Lower();

    // first try to find local variable
    const GameVarSpace* space = _e->local;
    if (space)
    {
        GameValue ret;
        bool found = space->VarGet(source, ret);
        if (found)
        {
            return ret;
        }
    }
    else
    {
        if (source[0] == '_')
        {
            if (s_emptyStatementKeepsValue)
            {
                GameValue ret;
                if (GlobalScratchLocals().VarGet(source, ret))
                    return ret;
                return GameValueNil();
            }
            SetError(EvalNamespace);
            return GameValueNil();
        }
    }

    const GameVariable& var = _vars[source];
    if (NotNull(var))
    {
        // get variable value
        return var._value;
    }

    return GameValueNil();
}

bool GameState::IsVisible(const GameVariable& var) const
{
    return var.IsReadOnly();
}

bool GameState::VarReadOnly(const char* name) const
{
    if (_e->_checkOnly)
    {
        // check reserved word list
        const char* reserved[] = {"this", "player", nullptr};
        for (const char** res = reserved; *res; res++)
        {
            if (!strcmp(*reserved, name))
            {
                return true;
            }
        }
        return false;
    }
    const GameVariable& var = _vars[name];
    if (IsNull(var))
    {
        return false;
    }
    return var._readOnly;
}

GameVarSpace::GameVarSpace() : _parent(nullptr) {}

GameVarSpace::GameVarSpace(GameVarSpace* parent) : _parent(parent) {}

void GameVarSpace::VarLocal(const char* name)
{
    const GameVariable& var = _vars[name];
    if (NotNull(var))
    {
        return;
    }

    GameVariable newvar(name, GameValueNil());
    _vars.Add(newvar);
}

void GameVarSpace::VarSet(const char* name, GameValuePar value, bool readOnly)
{
    // seek variable in this and all parents
    const GameVarSpace* seek = this;
    GameVariable* varFound = nullptr;
    const GameVarSpace* space = nullptr;
    while (seek)
    {
        const GameVariable& var = seek->_vars[name];
        if (NotNull(var))
        {
            varFound = const_cast<GameVariable*>(&var);
            space = seek;
            break;
        }
        seek = seek->_parent;
    }

    // we may destroy variables only in the outermost scope
    bool destroy = !readOnly && value.GetNil() && (!space || !space->_parent);
    if (varFound)
    {
        GameVariable& var = *varFound;

        // set variable value
        if (!destroy)
        {
            var._value = value;
            var._readOnly = readOnly;
        }
        else
        {
            _vars.Remove(name);
        }
    }
    else
    {
        // new variable
        if (!destroy)
        {
            GameVariable newvar(name, value, readOnly);
            _vars.Add(newvar);
        }
    }
}

bool GameVarSpace::VarGet(const char* name, GameValue& ret) const
{
    const GameVarSpace* seek = this;
    while (seek)
    {
        const GameVariable& var = seek->_vars[name];
        if (NotNull(var))
        {
            ret = var._value;
            return true;
        }
        seek = seek->_parent;
    }
    ret = GameValueNil();
    return false;
}

// forceLocal creates the variable in the local namespace even when its name
// does not start with an underscore.
void GameState::VarSet(const char* name, GameValuePar value, bool readOnly, bool forceLocal)
{
    // convert name to lower case
    RString source = name;
    source.Lower();

    GameVarSpace* space = (forceLocal || *name == '_' ? _e->local : this);
    if (!space && s_emptyStatementKeepsValue)
        space = &GlobalScratchLocals();
    if (!space)
    {
        SetError(EvalNamespace);
        return;
    }

    space->VarSet(source, value, readOnly);
}

void GameState::VarLocal(const char* name)
{
    GameVarSpace* space = _e->local;
    if (!space)
    {
        SetError(EvalNamespace);
        return;
    }
    RString source = name;
    source.Lower();
    space->VarLocal(source);
}

// The const variant only sets local variables: name must start with '_' or
// forceLocal must be set, otherwise it errors.
void GameState::VarSetLocal(const char* name, GameValuePar value, bool readOnly, bool forceLocal) const
{
    // convert name to lower case
    RString source = name;
    source.Lower();

    if (forceLocal || *name == '_')
    {
    }
    else
    {
        SetError(EvalBadVar);
        return;
    }
    GameVarSpace* space = _e->local;
    if (!space)
    {
        SetError(EvalNamespace);
        return;
    }
    space->VarLocal(source);
    space->VarSet(source, value, readOnly);
}

void GameState::VarDelete(const char* name)
{
    GameVarSpace* space = (*name == '_' ? _e->local : this);
    if (!space)
    {
        SetError(EvalNamespace);
        return;
    }
    space->_vars.Remove(name);
}

EvalError GameState::GetLastError() const
{
    return _e->_error;
}

RString GameState::GetLastErrorText() const
{
    return _e->_errorText;
}

int GameState::GetLastErrorPos() const
{
    return _e->_errorCarretPos;
}

GameValue GameState::Evaluate(const char* expression) const
{
    if (s_traceExpression)
        s_traceExpression(expression);
    // Snapshot _e on entry so any nested Evaluate triggered by an
    // operator handler (UI menu condition checks, animation rotations,
    // mission scripts, harness eval, etc.) leaves the outer caller's
    // parser state intact.  See VyhodFrameGuard above.  Guard wraps
    // the entire body — Vyhod + CleanStack + the _pos = nullptr
    // cleanup — so outer's slots aren't zeroed by our CleanStack and
    // outer's _pos isn't nulled by our cleanup.
    VyhodFrameGuard frameGuard(*_e);

    _e->_error = EvalOK;
    _e->_errorText = RString();
    _e->_pos0 = _e->_pos = expression;
    _e->_subexpBeg = expression;
    _e->_subexpEnd = expression + strlen(expression);

    GameValue result;
    if (*expression)
    {
        result = Vyhod();
        CleanStack();
        ShowError();
    }
    else
    {
        result = 0.0f;
    }

    return result;
}

static const char* CheckAssignment(const char* p)
{
    // Check for `private _identifier = value` shorthand (modern-SQF style).
    // strncmp (not memcmp) so a buffer shorter than 7 bytes stops at the NUL
    // instead of reading past it.
    if (strncmp(p, "private", 7) == 0 && !isalnumext(p[7]))
    {
        const char* q = p + 7;
        while (*q && isspace(*q))
            q++;
        if (*q == '_')
        {
            const char* r = q;
            while (*r && isalnumext(*r))
                r++;
            while (*r && isspace(*r))
                r++;
            if (r[0] == '=' && r[1] != '=')
                return r;
        }
    }
    // Standard `identifier = value` assignment.
    while (*p && isalnumext(*p))
    {
        p++;
    }
    while (*p && isspace(*p))
    {
        p++;
    }
    if (p[0] == '=' && p[1] != '=')
    {
        return p;
    }
    return nullptr;
}

// localOnly restricts assignments to local variables.
GameValue GameState::EvaluateMultiple(const char* expression, bool localOnly) const
{
    if (s_traceExpression)
        s_traceExpression(expression);
    // Reentrancy guard — see Evaluate above and VyhodFrameGuard.
    VyhodFrameGuard frameGuard(*_e);

    _e->_pos = _e->_pos0 = expression;
    _e->_subexpBeg = expression;
    _e->_subexpEnd = expression + strlen(expression);

    _e->_error = EvalOK;
    _e->_errorText = RString();

    GameValue ret = NOTHING;
    while (*_e->_pos)
    {
        if ((ret.GetType() & GameNothing) == 0)
        {
            if (_e->_checkOnly)
            {
                TypeError(GameNothing, ret.GetType());
                break;
            }
        }
        vynech();
        if (s_emptyStatementKeepsValue && (*_e->_pos == 0 || *_e->_pos == ';'))
        {
            if (*_e->_pos == 0)
                break;
            _e->_pos++;
            continue;
        }
        // check if expression is assignment
        // assignment must start with identifier
        _e->_subexpBeg = _e->_pos;
        if (CheckAssignment(_e->_pos))
        {
            if (!const_cast<GameState*>(this)->PerformAssignment(localOnly))
            {
                break;
            }
            ret = NOTHING;
        }
        else // non assignment
        {
            // check for empty command
            GameValue value = Vyhod();
            CleanStack();
            if (_e->_error)
            {
                // exitWith propagates its value even when breaking
                if (_e->_error == EvalBreak)
                    ret = value;
                break;
            }

            // check expression result
            // if result is nothing it's OK
            // if result is fake type it is not OK
            ret = value;
        } // single command executed
        vynech();
        char c = *_e->_pos;
        if (c == 0)
        {
            break;
        }
        if (c != ';' && (c != ',' || _e->_checkOnly))
        {
            SetError(EvalSemicolon);
            break;
        }
        _e->_pos++;
    }
    ShowError();
    // _pos / _pos0 / _subexpBeg / _subexpEnd are restored by frameGuard on scope exit.
    return ret;
}

bool GameState::EvaluateBool(const char* expression) const
{
    GameValue value = EvaluateMultiple(expression);
    if ((value.GetType() & GameBool) == 0)
    {
        TypeError(GameBool, value.GetType());
        return false;
    }
    return bool(value);
}

bool GameState::EvaluateMultipleBool(const char* expression, bool localOnly) const
{
    GameValue value = EvaluateMultiple(expression, localOnly);
    if (value.GetType() & GameBool)
    {
        return bool(value);
    }
    TypeError(GameBool, value.GetType());
    return false;
}

bool GameState::VarGoodName(const char* name) const
{
    return true;
}

bool GameState::IdtfGoodName(const char* name) const
{
    if (!isalphaext(*name))
    {
        return false;
    }
    for (const char* n = name; *n; n++)
    {
        if (!isalnumext(*n))
        {
            return false;
        }
    }
    return VarGoodName(name);
}

bool GameState::LValueGoodName(const char* name) const
{
    if (VarReadOnly(name))
    {
        return false;
    }
    return VarGoodName(name);
}

// localOnly restricts assignments to local variables.
bool GameState::PerformAssignment(bool localOnly)
{
    // Handle `private _identifier = value` shorthand — declare the variable
    // as local before performing the assignment.
    bool declareLocal = false;
    // strncmp (not memcmp) so a buffer shorter than 7 bytes stops at the NUL.
    if (strncmp(_e->_pos, "private", 7) == 0 && !isalnumext(_e->_pos[7]))
    {
        declareLocal = true;
        _e->_pos += 7;
        vynech();
    }

    char name[MAX_EXPR_LEN];
    char* wName = name;
    if (isalphaext(*_e->_pos))
    {
        int i = 0;
        while (isalnumext(*_e->_pos))
        {
            if (i < MAX_EXPR_LEN - 1)
            {
                *wName++ = *_e->_pos, i++;
            }
            _e->_pos++;
        }
    }
    *wName = 0;
    if (!*name)
    {
        SetError(EvalBadVar);
        return false;
    }
    vynech();
    if (*_e->_pos != '=')
    {
        SetError(EvalEqu);
        return false;
    }

    strlwr(name);

    if (declareLocal)
    {
        VarLocal(name);
        localOnly = true;
    }

    if (!LValueGoodName(name))
    {
        SetError(EvalBadVar);
        return false;
    }

    _e->_pos++;
    vynech();

    GameValue value = Vyhod();
    CleanStack();
    if (_e->_error)
    {
        return false;
    }
    if (_e->_checkOnly)
    {
        if (value.GetType() == GameNothing)
        {
            TypeError(GameAny, value.GetType());
            return false;
        }
        if (value.GetType() & FakeTypes)
        {
            TypeError(GameAny, value.GetType());
        }
    }
    if (!_e->_checkOnly)
    {
        if (value.SharedInstance() && value.GetType() != GameArray)
        {
            value.Duplicate(value);
        }
        if (localOnly)
        {
            VarSetLocal(name, value);
        }
        {
            VarSet(name, value);
        }
    }
    return true;
}

void GameState::Execute(const char* expression)
{
    if (s_traceExpression)
        s_traceExpression(expression);
    // Reentrancy guard — see Evaluate above and VyhodFrameGuard.
    VyhodFrameGuard frameGuard(*_e);

    _e->_error = EvalOK;
    _e->_errorText = RString();

    if (expression[0] == 0)
    {
        return;
    }
    _e->_pos = _e->_pos0 = expression;
    _e->_subexpBeg = expression;
    _e->_subexpEnd = expression + strlen(expression);

    while (*_e->_pos)
    {
        vynech();
        // check if expression is assignment
        // assignment must start with identifier
        _e->_subexpBeg = _e->_pos;
        if (CheckAssignment(_e->_pos))
        {
            if (!PerformAssignment())
            {
                break;
            }
        }
        else // non assignment
        {
            GameValue value = Vyhod();
            CleanStack();
            if (_e->_error)
            {
                break;
            }

            // check expression result
            // if result is nothing it's OK
            if ((value.GetType() & GameNothing) == 0)
            {
                // otherwise we may issue error
                if (_e->_checkOnly)
                {
                    TypeError(GameNothing, value.GetType());
                    break;
                }
            }
        } // single command executed
        vynech();
        char c = *_e->_pos;
        if (c == 0)
        {
            break;
        }
        if (c != ';' && (c != ',' || _e->_checkOnly))
        {
            SetError(EvalSemicolon);
            break;
        }
        _e->_pos++;
    }
    ShowError();
    _e->_pos = nullptr;
    _e->_pos0 = nullptr;
    _e->_subexpBeg = nullptr;
    _e->_subexpEnd = nullptr;
}

void GameState::ShowError() const
{
    if (_e->_error == EvalBreak)
        return; // control flow — not a display error; loop handlers clear it
    if (_e->_error)
    {
        // show message only when running in editor
        const char* start = _e->_subexpBeg ? _e->_subexpBeg : _e->_pos0;
        const char* end = _e->_subexpEnd ? _e->_subexpEnd : _e->_pos0 + strlen(_e->_pos0);
        if (end > _e->_pos + 256)
        {
            end = _e->_pos + 256;
        }
        RString expr(start, end - start);
        RString before(start, _e->_pos - start);
        RString after(_e->_pos, end - _e->_pos);
        RString buf = before + RString("|#|") + after;

        DefaultInfoFunctions()->DisplayErrorMessage(buf, _e->_errorText);
        RptF("Error in expression <%s>", (const char*)expr);
        RptF("  Error position: <%s>", (const char*)after);
        RptF("  Error %s", (const char*)_e->_errorText);
    }
}

GameEvaluator::GameEvaluator(GameVarSpace* vars) : _stack(SL), UB(SL), _priorStack(SL), _operStack(SL)
{
    local = vars;
    // Initialize parser-private fields so VyhodFrameGuard's snapshot of an
    // unused EvalState reads well-defined values: the guard snapshots before
    // Vyhod resets them on entry, so these must already hold valid defaults.
    SP = 0;
    _parPrior = 0;
    _listPrior = 0;
    _checkOnly = false;
    _error = EvalOK;
    _errorCarretPos = 0;
    _pos = nullptr;
    _pos0 = nullptr;
    _subexpBeg = nullptr;
    _subexpEnd = nullptr;
}

GameEvaluator::~GameEvaluator() = default;

GameVarSpace* GameState::GetContext() const
{
    return _e->local;
}

void GameState::BeginContext(GameVarSpace* vars) const
{
    if (_actContext >= MaxContexts)
    {
        Poseidon::Foundation::ErrorMessage("GameState context stack overflow");
        return;
    }
    _actContext++;
    _contextStack[_actContext] = new GameEvaluator(vars);
    _e = _contextStack[_actContext];

    _e->_error = EvalOK;
    _e->_errorText = RString();
    _e->_checkOnly = false;
    _e->_errorCarretPos = 0;
}

void GameState::EndContext() const
{
    if (_actContext <= 0)
    {
        Poseidon::Foundation::ErrorMessage("GameState context stack underflow");
        return;
    }
    _contextStack[_actContext].Free();
    _actContext--;
    _e = _contextStack[_actContext];
}

bool GameState::CheckEvaluate(const char* expression) const
{
    _e->_checkOnly = true;
    Evaluate(expression);
    _e->_checkOnly = false;
    bool ret = (_e->_error == EvalOK);
    return ret;
}

bool GameState::CheckEvaluateBool(const char* expression) const
{
    _e->_checkOnly = true;
    EvaluateBool(expression);
    _e->_checkOnly = false;
    bool ret = (_e->_error == EvalOK);
    return ret;
}

bool GameState::CheckExecute(const char* expression) const
{
    _e->_checkOnly = true;
    const_cast<GameState*>(this)->Execute(expression);
    _e->_checkOnly = false;
    bool ret = (_e->_error == EvalOK);
    return ret;
}

#ifndef ACCESS_ONLY
LSError GameVariable::Serialize(ParamArchive& ar)
{
    CHECK(GameState::Serialize(ar, "name", _name, 1))
    CHECK(_value.Serialize(ar));
    CHECK(GameState::Serialize(ar, "readOnly", _readOnly, 1))
    return (LSError)0;
}
#endif

GameData* GameState::CreateGameData(GameType type) const
{
    // GameVoid is static type
    if (type == GameVoid)
    {
        return new GameDataNil(GameVoid);
    }
    if (type == GameAny)
    {
        return new GameDataNil(GameAny);
    }

    for (int i = 0; i < _typeNames.Size(); i++)
    {
        if (_typeNames[i].value == type)
        {
            if (_typeNames[i].createFunction)
            {
                return _typeNames[i].createFunction();
            }
            Fail("Missing create function");
            return nullptr;
        }
    }
    Fail("Unknown data type");
    return nullptr;
}

static void CatType(RString& a, RString b)
{
    if (a.GetLength() <= 0)
    {
        a = b;
    }
    else if (b.GetLength() <= 0)
    {
        ;
    }
    else
    {
        a = a + RString(",") + b;
    }
}

RString GameState::GetTypeName(GameType type) const
{
    // GameVoid is static type
    if (type == GameVoid)
    {
        return DefaultInfoFunctions()->GetTypeName(GameVoid);
    }
    if (type == GameAny)
    {
        return DefaultInfoFunctions()->GetTypeName(GameVoid);
    }
    if (type == GameNothing)
    {
        return DefaultInfoFunctions()->GetTypeName(GameNothing);
    }

    RString ret = "";
    if ((type & GameAny) == GameAny)
    {
        CatType(ret, DefaultInfoFunctions()->GetTypeName(GameVoid));
        type &= ~GameAny;
    }
    if ((type & GameVoid) == GameVoid)
    {
        CatType(ret, DefaultInfoFunctions()->GetTypeName(GameVoid));
        type &= ~GameVoid;
    }
    for (int i = 0; i < _typeNames.Size(); i++)
    {
        if (type & (_typeNames[i].value))
        {
            CatType(ret, _typeNames[i].typeName);
        }
    }

    return ret;
}

GameValue GameState::CreateGameValue(GameType type) const
{
    GameData* data = CreateGameData(type);
    if (!data)
    {
        return GameValue();
    }
    return GameValue(data);
}

#ifndef ACCESS_ONLY
LSError GameState::Serialize(ParamArchive& ar)
{
    return DefaultArchiveFunctions()->Serialize(ar, this);
}
#endif
