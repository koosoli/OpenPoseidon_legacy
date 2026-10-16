#pragma once
#include <string>
#include <algorithm>
#include <vector>

#ifndef _express_hpp
#define _express_hpp

#include <Poseidon/Foundation/Strings/RString.hpp>

#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <Poseidon/IO/Serialization/SerializeClass.hpp>

using Poseidon::Foundation::AutoArray;
using Poseidon::Foundation::Ref;
using Poseidon::Foundation::RefCount;
using Poseidon::Foundation::RString;

enum EvalError
{ // see also: "stringIds.hpp" and "stringtable.csv" (EVAL_...)
    EvalOK,
    EvalGen,       // generic error
    EvalExpo,      // exponent out of range or invalid
    EvalNum,       // invalid number
    EvalVar,       // undefined variable
    EvalBadVar,    // undefined variable
    EvalDivZero,   // zero divisor
    EvalTg90,      // tg 90
    EvalOpenB,     // missing (
    EvalCloseB,    // missing )
    EvalEqu,       // missing =
    EvalSemicolon, // missing =
    EvalOper,      // unknown operator
    EvalLineLong,  // line too long
    EvalType,      // type mismatch
    EvalNamespace, // no name space
    EvalDim,       // array dimension
    EvalBreak,     // control flow: exitWith triggered (not a display error)
};

#include <Poseidon/Foundation/Containers/HashMap.hpp>

#define DERIVED(type, basic) typedef basic type;

DERIVED(GameType, int);

const GameType GameScalar(1);
const GameType GameArray(2);
const GameType GameBool(4);
const GameType GameString(8);
const GameType GameNothing(16);
const GameType GameIf(0x1000000);
const GameType GameWhile(0x2000000);
const GameType GameFor(0x4000000);
const GameType GameForFrom(0x8000000);
const GameType GameForTo(0x10000000);

const GameType FakeTypes(GameIf | GameWhile | GameFor | GameForFrom | GameForTo);
const GameType GameVoid(~(GameNothing | FakeTypes)); // any value type
const GameType GameAny(~FakeTypes);                  // any type including nothing

#ifndef ACCESS_ONLY
class ParamArchive;

LSError Serialize(ParamArchive& ar, RString name, GameType& value, int minVersion);
#endif

class GameValue;

typedef float GameScalarType;
typedef bool GameBoolType;
typedef RString GameStringType;
typedef AutoArray<GameValue> GameArrayType;

// Meyers singletons - no global constructors
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"

inline const GameArrayType& GameArrayEmpty()
{
    static const GameArrayType instance;
    return instance;
}

inline GameArrayType& GameArrayDummy()
{
    static GameArrayType instance;
    return instance;
}

#pragma clang diagnostic pop

class GameData : public RefCount
{
  public:
    virtual GameType GetType() const = 0;
    ~GameData() override = default;

    virtual GameBoolType GetBool() const { return false; }
    virtual GameScalarType GetScalar() const { return 0; }
    virtual GameStringType GetString() const { return ""; }
    virtual const GameArrayType& GetArray() const { return GameArrayEmpty(); }
    virtual GameArrayType& GetArray() { return GameArrayDummy(); }
    virtual GameData* Clone() const { return nullptr; }
    //! if there are any shared data (see GameDataArray), we may wish to set them as read only
    virtual void SetReadOnly(bool /*val*/) {}
    //! check read only status
    virtual bool GetReadOnly() const { return false; }

    virtual RString GetText() const = 0;
    virtual bool IsEqualTo(const GameData* data) const = 0;

    virtual const char* GetTypeName() const { return "unknown"; }
    virtual bool GetNil() const { return false; }
    //! a code block ({...}) rather than a string (see GameDataCode)
    virtual bool IsCode() const { return false; }

#ifndef ACCESS_ONLY
    static GameData* CreateObject(ParamArchive& ar);
    virtual LSError Serialize(ParamArchive& ar);
#endif
};

class GameDataNil : public GameData
{
    GameType _type;

  public:
    GameDataNil(GameType type) : _type(type) {}
    GameType GetType() const override { return _type; }

    RString GetText() const override;
    bool IsEqualTo(const GameData* data) const override;
    bool GetNil() const override { return true; }
    const char* GetTypeName() const override { return "void"; }
    GameData* Clone() const override { return new GameDataNil(*this); }

#ifndef ACCESS_ONLY
    LSError Serialize(ParamArchive& ar) override;
#endif

    USE_FAST_ALLOCATOR
};

class GameDataScalar : public GameData
{
    typedef GameData base;

    GameScalarType _value;

  public:
    GameDataScalar() : _value(0) {}
    GameDataScalar(GameScalarType value) : _value(value) {}
    ~GameDataScalar() override = default;

    GameType GetType() const override { return GameScalar; }
    GameScalarType GetScalar() const override { return _value; }

    RString GetText() const override;
    bool IsEqualTo(const GameData* data) const override;
    const char* GetTypeName() const override { return "float"; }
    GameData* Clone() const override { return new GameDataScalar(*this); }

#ifndef ACCESS_ONLY
    LSError Serialize(ParamArchive& ar) override;
#endif

    USE_FAST_ALLOCATOR
};

class GameDataNothing : public GameData
{
    typedef GameData base;

  public:
    GameDataNothing() = default;
    ~GameDataNothing() override = default;

    GameType GetType() const override { return GameNothing; }

    RString GetText() const override { return "nothing"; }
    bool IsEqualTo(const GameData* data) const override;
    const char* GetTypeName() const override { return "nothing"; }
    GameData* Clone() const override { return new GameDataNothing(*this); }

    USE_FAST_ALLOCATOR
};

class GameDataString : public GameData
{
    typedef GameData base;

    GameStringType _value;

  public:
    GameDataString() : _value(nullptr) {}
    GameDataString(GameStringType value) : _value(value) {}
    ~GameDataString() override = default;

    GameType GetType() const override { return GameString; }
    GameStringType GetString() const override { return _value; }

    RString GetText() const override;
    bool IsEqualTo(const GameData* data) const override;
    const char* GetTypeName() const override { return "float"; }
    GameData* Clone() const override { return new GameDataString(*this); }

#ifndef ACCESS_ONLY
    LSError Serialize(ParamArchive& ar) override;
#endif

    USE_FAST_ALLOCATOR
};

// A code block ({...}) when GameState::s_codeValues is set (Arma 3 content). It is still a
// string to every command that takes code -- this engine runs code from its text -- but
// typeName, isEqualType and params can tell it from a string, as Arma 3's scripts expect.
class GameDataCode : public GameDataString
{
  public:
    GameDataCode() = default;
    GameDataCode(GameStringType value) : GameDataString(value) {}
    bool IsCode() const override { return true; }
    RString GetText() const override;
    const char* GetTypeName() const override { return "code"; }
    GameData* Clone() const override { return new GameDataCode(*this); }
};

class GameDataBool : public GameData
{
    typedef GameData base;

    GameBoolType _value;

  public:
    GameDataBool() : _value(false) {}
    GameDataBool(GameBoolType value) : _value(value) {}
    ~GameDataBool() override = default;

    GameType GetType() const override { return GameBool; }
    GameBoolType GetBool() const override { return _value; }
    GameScalarType GetScalar() const override { return _value; }

    RString GetText() const override;
    bool IsEqualTo(const GameData* data) const override;
    const char* GetTypeName() const override { return "bool"; }
    GameData* Clone() const override { return new GameDataBool(*this); }

#ifndef ACCESS_ONLY
    LSError Serialize(ParamArchive& ar) override;
#endif

    USE_FAST_ALLOCATOR
};

class GameDataIf : public GameDataBool
{
  public:
    GameDataIf() = default;
    GameDataIf(GameBoolType value) : GameDataBool(value) {}
    GameType GetType() const override { return GameIf; }
    const char* GetTypeName() const override { return "if"; }
    GameData* Clone() const override { return new GameDataIf(*this); }
};

class GameDataWhile : public GameDataString
{
  public:
    GameDataWhile() = default;
    GameDataWhile(GameStringType value) : GameDataString(value) {}
    GameType GetType() const override { return GameWhile; }
    const char* GetTypeName() const override { return "while"; }
    GameData* Clone() const override { return new GameDataWhile(*this); }
};

// for "var" — holds the loop variable name
class GameDataFor : public GameDataString
{
  public:
    GameDataFor() = default;
    GameDataFor(GameStringType value) : GameDataString(value) {}
    GameType GetType() const override { return GameFor; }
    const char* GetTypeName() const override { return "for"; }
    GameData* Clone() const override { return new GameDataFor(*this); }
};

// for "var" from N — holds variable name + start value
class GameDataForFrom : public GameData
{
    RString m_var;
    float m_from;

  public:
    GameDataForFrom() : m_from(0) {}
    GameDataForFrom(const char* var, float from) : m_var(var), m_from(from) {}
    GameType GetType() const override { return GameForFrom; }
    const char* GetTypeName() const override { return "for-from"; }
    GameData* Clone() const override { return new GameDataForFrom(*this); }
    RString GetText() const override { return "for-from"; }
    bool IsEqualTo(const GameData*) const override { return false; }
    const char* GetVar() const { return m_var; }
    float GetFrom() const { return m_from; }
};

// for "var" from N to M [step S] — holds variable, range, and step
class GameDataForTo : public GameData
{
    RString m_var;
    float m_from;
    float m_to;
    float m_step;

  public:
    GameDataForTo() : m_from(0), m_to(0), m_step(1) {}
    GameDataForTo(const char* var, float from, float to, float step = 1.0f)
        : m_var(var), m_from(from), m_to(to), m_step(step)
    {
    }
    GameType GetType() const override { return GameForTo; }
    const char* GetTypeName() const override { return "for-to"; }
    GameData* Clone() const override { return new GameDataForTo(*this); }
    RString GetText() const override { return "for-to"; }
    bool IsEqualTo(const GameData*) const override { return false; }
    const char* GetVar() const { return m_var; }
    float GetFrom() const { return m_from; }
    float GetTo() const { return m_to; }
    float GetStep() const { return m_step; }
};

class GameDataArray : public GameData
{
    typedef GameData base;

    //! actual array data
    GameArrayType _value;
    //! some array may be read only
    bool _readOnly;

  public:
    GameDataArray();
    GameDataArray(const GameArrayType& value);
    ~GameDataArray() override;

    GameType GetType() const override { return GameArray; }
    const GameArrayType& GetArray() const override { return _value; }
    GameArrayType& GetArray() override { return _value; }
    //! change or crate given element
    void SetReadOnly(bool val) override { _readOnly = val; }
    bool GetReadOnly() const override { return _readOnly; }

    RString GetText() const override;
    bool IsEqualTo(const GameData* data) const override;
    const char* GetTypeName() const override { return "array"; }
    GameData* Clone() const override;

#ifndef ACCESS_ONLY
    LSError Serialize(ParamArchive& ar) override;
#endif

    USE_FAST_ALLOCATOR
};

// GameValue intentionally uses non-virtual destructor despite virtual functions
// This is safe because destruction is always through concrete type
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdelete-non-abstract-non-virtual-dtor"
#pragma clang diagnostic ignored "-Wdynamic-class-memaccess"

class GameValue : public SerializeClass
{
    friend class GameState;
    friend class GameVariable;

    // only one of these should be valid
  protected:
    Ref<GameData> _data;

  public:
    explicit GameValue(GameData* data);

  public:
    GameValue();

    GameValue(GameBoolType value) { _data = new GameDataBool(value); }
    GameValue(GameScalarType value) { _data = new GameDataScalar(value); }
    GameValue(const GameArrayType& value) { _data = new GameDataArray(value); }
    GameValue(GameStringType value) { _data = new GameDataString(value); }
    GameValue(const char* value) { _data = new GameDataString(value); }

    void operator=(bool value) { _data = new GameDataBool(value); }
    void operator=(float value) { _data = new GameDataScalar(value); }
    void operator=(const GameArrayType& value) { _data = new GameDataArray(value); }
    void operator=(GameStringType value) { _data = new GameDataString(value); }
    void operator=(const char* value) { _data = new GameDataString(value); }

    // special handling of copy
    GameValue(const GameValue& value);
    void operator=(const GameValue& value);

    void Duplicate(const GameValue& value);
    bool SharedInstance() const { return _data ? _data->RefCounter() > 1 : false; }

    // access
    GameData* GetData() const { return _data; }
    GameType GetType() const { return _data ? _data->GetType() : GameAny; }
    bool GetNil() const { return _data ? _data->GetNil() : true; }

    operator GameBoolType() const { return _data ? _data->GetBool() : false; }
    operator GameScalarType() const { return _data ? _data->GetScalar() : 0; }
    operator const GameArrayType&() const { return _data ? _data->GetArray() : GameArrayEmpty(); }
    operator GameArrayType&() { return _data ? _data->GetArray() : GameArrayDummy(); }
    operator GameStringType() const { return _data ? _data->GetString() : GameStringType(""); }

    bool GetReadOnly() const { return _data ? _data->GetReadOnly() : false; }
    void SetReadOnly(bool val)
    {
        if (_data)
        {
            _data->SetReadOnly(val);
        }
    }

    RString GetText() const;
    RString GetDebugText() const;
    bool IsEqualTo(const GameValue& value) const;

#ifndef ACCESS_ONLY
    LSError Serialize(ParamArchive& ar) override;
#else
    LSError Serialize(ParamArchive& ar) { return LSOK; }
#endif
};

#pragma clang diagnostic pop

    typedef const GameValue& GameValuePar;

class GameVariable
{
  public:
    RString _name;
    GameValue _value;
    bool _readOnly;

    GameVariable() : _name(nullptr), _readOnly(false) {}
    GameVariable(RString name, GameValuePar value, bool readOnly = false) : _name(name), _readOnly(readOnly)
    {
        _value = value;
    };
    const char* GetKey() const { return _name; }
    LSError Serialize(ParamArchive& ar);

    RString GetName() const { return _name; }
    RString GetValueText() const { return _value.GetText(); }

    bool IsReadOnly() const { return _readOnly; }
};


typedef MapStringToClass<GameVariable, AutoArray<GameVariable>> VarBankType;

// typedef double Real;
typedef float Real;

#define SL 256 // stack levels (initial)
#define SLMax 65536 // stack levels (most)

class GameState;
typedef GameValue (*ProcBinary)(const GameState* state, GameValuePar o1, GameValuePar o2);
typedef GameValue (*ProcUnary)(const GameState* state, GameValuePar o1);
typedef GameValue (*ProcNular)(const GameState* state);

struct GameOpName
{
    RString _opName;
    GameOpName() = default;
    GameOpName(RString name) : _opName(name) {}
};

struct GameNular : public GameOpName
{
    RString _name;
    ProcNular _proc;
    GameType _retType;

    GameNular() = default;
    GameNular(GameType retType, RString name, ProcNular proc)
        : GameOpName(name), _name(name), _proc(proc), _retType(retType)
    {
        _name.Lower();
    }
    const char* GetKey() const { return _name; }
};

    typedef MapStringToClass<GameNular, AutoArray<GameNular>> GameNularsType;

struct GameFunction : public GameOpName
{
    RString _name;
    ProcUnary _proc;
    GameType _retType;
    GameType _argType;

    GameFunction() = default;
    GameFunction(GameType retType, RString name, ProcUnary proc, GameType argType)
        : GameOpName(name), _name(name), _proc(proc), _retType(retType), _argType(argType)
    {
        _name.Lower();
    }
};

    struct GameFunctions : public AutoArray<GameFunction>,
                           public GameOpName
{
    RString _name;
    GameFunctions() = default;
    GameFunctions(RString name) : GameOpName(name), _name(name) { _name.Lower(); }
    const char* GetKey() const { return _name; }
};

    typedef MapStringToClass<GameFunctions, AutoArray<GameFunctions>> GameFunctionsType;

enum GamePriority
{
    nula,
    logicOr,
    logicAnd,
    comparison,
    function,
    functionFirst,
    soucet,
    soucin,
    unar,
    mocnina,
    zavorky
};

struct GameOperator : public GameOpName
{
    RString _name;
    ProcBinary _proc;
    GamePriority _priority;
    GameType _retType;
    GameType _argType1, _argType2;

    GameOperator() = default;
    GameOperator(GameType retType, RString name, GamePriority priority, ProcBinary proc, GameType argType1,
                 GameType argType2)
        : GameOpName(name), _name(name), _proc(proc), _priority(priority), _retType(retType), _argType1(argType1),
          _argType2(argType2)
    {
        _name.Lower();
    }
};

    struct GameOperators : public AutoArray<GameOperator>,
                           public GameOpName
{
    RString _name;
    GamePriority _priority;
    GameOperators() = default;
    GameOperators(RString name, GamePriority priority) : GameOpName(name), _name(name), _priority(priority)
    {
        _name.Lower();
    }
    const char* GetKey() const { return _name; }
};

    typedef MapStringToClass<GameOperators, AutoArray<GameOperators>> GameOperatorsType;

typedef MapStringToClass<RString, AutoArray<RString>> GameNameType;

class GameVarSpace
{
  public:
    VarBankType _vars;
    const GameVarSpace* _parent;

    //! no local variables inherited
    GameVarSpace();
    //! inherit all local variables from some parent scope
    GameVarSpace(GameVarSpace* parent);

    static bool IsNull(const GameVariable& value) { return VarBankType::IsNull(value); }
    static bool NotNull(const GameVariable& value) { return VarBankType::NotNull(value); }
    static GameVariable& Null() { return VarBankType::Null(); }

    //! set variable in this and all parent scopes
    void VarSet(const char* name, GameValuePar value, bool readOnly = false);
    //! get variable in this and all parent scopes
    bool VarGet(const char* name, GameValue& ret) const;
    //! create undefined variable in the innermost scope
    void VarLocal(const char* name);
};

struct VyhodFrameGuard;

class GameEvaluator : public RefCount
{
    friend class GameState;
    friend struct VyhodFrameGuard;
    friend struct ScopeNameGuard;

    GameVarSpace* local; // local variables

    const char *_pos, *_pos0;
    const char *_subexpBeg, *_subexpEnd;
    // Parser stacks. They start at SL entries and grow (up to SLMax) when an expression
    // needs more -- a literal array pushes every element before it is collected, and
    // Arma 2 mission scripts hold arrays of thousands of values.
    std::vector<GameValue> _stack; // value stack
    std::vector<int> UB;           // unary/binary flag
    std::vector<int> _priorStack;  // prioriry stack
    std::vector<RString> _operStack;
    bool Reserve(int sp)
    {
        if (sp < static_cast<int>(_stack.size()))
            return true;
        if (static_cast<int>(_stack.size()) >= SLMax)
            return false;
        const size_t n = std::min<size_t>(_stack.size() * 2, SLMax);
        _stack.resize(n);
        UB.resize(n);
        _priorStack.resize(n);
        _operStack.resize(n);
        return true;
    }
    int SP;         // stack pointer
    int _parPrior;  // parenthesis priority
    int _listPrior; // parenthesis priority
    bool _checkOnly;
    EvalError _error;
    RString _errorText; // user friendly error text
    int _errorCarretPos;
    RString _scopeName; // scopeName of the scope this evaluator runs (Arma 2)

    GameEvaluator(GameVarSpace* vars = nullptr);
    ~GameEvaluator() override;

    USE_FAST_ALLOCATOR
};

//! class of callback functions
class ArchiveFunctions
{
  public:
    ArchiveFunctions() = default;
    virtual ~ArchiveFunctions() = default;

    //! callback function to serialize boolean value
    virtual LSError Serialize(ParamArchive& /*ar*/, const RStringB& /*name*/, bool& /*value*/, int /*minVersion*/)
    {
        return (LSError)0;
    }
    //! callback function to serialize boolean value
    virtual LSError Serialize(ParamArchive& /*ar*/, const RStringB& /*name*/, bool& /*value*/, int /*minVersion*/,
                              bool /*defValue*/)
    {
        return (LSError)0;
    }
    //! callback function to serialize GameType value
    virtual LSError Serialize(ParamArchive& /*ar*/, const RStringB& /*name*/, GameType& /*value*/, int /*minVersion*/)
    {
        return (LSError)0;
    }
    //! callback function to serialize float value
    virtual LSError Serialize(ParamArchive& /*ar*/, const RStringB& /*name*/, float& /*value*/, int /*minVersion*/)
    {
        return (LSError)0;
    }
    //! callback function to serialize string value
    virtual LSError Serialize(ParamArchive& /*ar*/, const RStringB& /*name*/, RString& /*value*/, int /*minVersion*/)
    {
        return (LSError)0;
    }
    //! callback function to serialize array value
    virtual LSError Serialize(ParamArchive& /*ar*/, const RStringB& /*name*/, AutoArray<GameValue>& /*value*/,
                              int /*minVersion*/)
    {
        return (LSError)0;
    }
    //! callback function to serialize game data value
    virtual LSError Serialize(ParamArchive& /*ar*/, const RStringB& /*name*/, Ref<GameData>& /*value*/,
                              int /*minVersion*/)
    {
        return (LSError)0;
    }
    //! callback function to serialize game state
    virtual LSError Serialize(ParamArchive& /*ar*/, GameState* /*value*/) { return (LSError)0; }
    //! callback function to create game data
    virtual GameData* CreateGameData(ParamArchive& /*ar*/, GameType /*type*/) { return nullptr; }
    //! callback function to check load/save status of archive
    virtual bool IsSaving(ParamArchive& /*ar*/) { return false; }
};

//! class of callback functions
class GameStateInfoFunctions
{
  public:
    GameStateInfoFunctions() = default;
    virtual ~GameStateInfoFunctions() = default;

    //! callback function to return error description
    virtual const char* GetErrorString(EvalError /*error*/) const { return ""; }
    //! callback function to return type name
    virtual RString GetTypeName(GameType /*type*/) const { return ""; }
    //! callback function to display error message
    virtual void DisplayErrorMessage(const char* /*position*/, const char* /*error*/) const {}

    //! callback function to display debug info
    virtual void DisplayDebugMessage(const char* /*content*/, int /*timeMs*/) const {}
};

typedef GameData* CreateGameDataFunction();

struct GameTypeType : public Poseidon::Foundation::EnumName
{
    CreateGameDataFunction* createFunction;
    RString typeName;

    GameTypeType(int v, RStringB n, CreateGameDataFunction* f, RString tname) : Poseidon::Foundation::EnumName(v, n)
    {
        createFunction = f;
        typeName = tname;
    }
    GameTypeType() { createFunction = nullptr; }
    GameData* CreateGameData() { return createFunction(); }
};

    class GameState : public SerializeClass,
                      public GameVarSpace
{
  private:
    AutoArray<GameTypeType> _typeNames; // symbolic name list

    GameFunctionsType _functions;
    GameOperatorsType _operators;
    GameNularsType _nulars;

    enum
    {
        MaxContexts = 64
    };
    mutable Ref<GameEvaluator> _contextStack[MaxContexts];
    mutable int _actContext;

    mutable GameEvaluator* _e;

    static ArchiveFunctions* _defaultArchiveFunctions;
    static GameStateInfoFunctions* _defaultInfoFunctions;

  protected:
    bool VerifyArg(GameValuePar par, GameType type);

    void vynech() const;
    Real sejmid() const;

    void VyhCast(int Prio) const; // evaluate to priority
    GameValue Vyhod() const;      // partial evaluation
    GameValue Const() const;      // term evaluation
    void CleanStack() const;

    // report argument types incompatible
    void TypeErrorOperator(const char* name, GameType left, GameType right) const;
    void TypeErrorFunction(const char* name, GameType right) const;

  public:
  public:
    void NewNularOp(const GameNular& f);
    void NewFunction(const GameFunction& f);
    void NewOperator(const GameOperator& f);

    void NewNularOps(const GameNular* f, int count);
    void NewFunctions(const GameFunction* f, int count);
    void NewOperators(const GameOperator* f, int count);

    //! append all nular op names passed by filter to given string array
    void AppendNularOpList(AutoArray<RStringS>& dic, bool (*filter)(const char* word)) const;
    //! append all function names passed by filter to given string array
    void AppendFunctionList(AutoArray<RStringS>& dic, bool (*filter)(const char* word)) const;
    //! append all operator names passed by filter to given string array
    void AppendOperatorList(AutoArray<RStringS>& dic, bool (*filter)(const char* word)) const;

    // name must exist during whole existance of GameState object
    // string constant expected
    void NewType(const char* name, GameType val, CreateGameDataFunction* func, RString tname);

  public:
    GameState();
    ~GameState();

    void Init();
    void Reset();

    static void SetDefaultArchiveFunctions(ArchiveFunctions* f) { _defaultArchiveFunctions = f; }

    // Lazy fallback: if ExpressExt.cpp's GParamArchiveFunctions constructor
    // hasn't run yet (or the TU got dropped by the linker entirely),
    // _defaultArchiveFunctions is nullptr — fall back to a Meyer's
    // singleton no-op base.  The eventual SetDefaultArchiveFunctions call
    // from ExpressExt.cpp replaces it, so steady-state behaviour matches
    // the real impl.  See express.cpp for the SIOF rationale.
    static ArchiveFunctions* DefaultArchiveFunctions()
    {
        if (_defaultArchiveFunctions)
            return _defaultArchiveFunctions;
        static ArchiveFunctions fallback;
        return &fallback;
    }

    static LSError Serialize(ParamArchive& ar, const RStringB& name, bool& value, int minVersion)
    {
        return DefaultArchiveFunctions()->Serialize(ar, name, value, minVersion);
    }

    static LSError Serialize(ParamArchive& ar, const RStringB& name, bool& value, int minVersion, bool defValue)
    {
        return DefaultArchiveFunctions()->Serialize(ar, name, value, minVersion, defValue);
    }

    static LSError Serialize(ParamArchive& ar, const RStringB& name, GameType& value, int minVersion)
    {
        return DefaultArchiveFunctions()->Serialize(ar, name, value, minVersion);
    }

    static LSError Serialize(ParamArchive& ar, const RStringB& name, float& value, int minVersion)
    {
        return DefaultArchiveFunctions()->Serialize(ar, name, value, minVersion);
    }

    static LSError Serialize(ParamArchive& ar, const RStringB& name, RString& value, int minVersion)
    {
        return DefaultArchiveFunctions()->Serialize(ar, name, value, minVersion);
    }

    static LSError Serialize(ParamArchive& ar, const RStringB& name, AutoArray<GameValue>& value, int minVersion)
    {
        return DefaultArchiveFunctions()->Serialize(ar, name, value, minVersion);
    }

    static LSError Serialize(ParamArchive& ar, const RStringB& name, Ref<GameData>& value, int minVersion)
    {
        return DefaultArchiveFunctions()->Serialize(ar, name, value, minVersion);
    }

    static LSError Serialize(ParamArchive& /*ar*/, const RStringB& /*name*/, int /*minVersion*/) { return (LSError)0; }
    static LSError Serialize(ParamArchive& /*ar*/, const RStringB& /*name*/, int /*minVersion*/, bool /*defValue*/)
    {
        return (LSError)0;
    }

    static GameData* CreateGameData(ParamArchive& ar, GameType type)
    {
        return DefaultArchiveFunctions()->CreateGameData(ar, type);
    }
    static bool IsSaving(ParamArchive& ar) { return DefaultArchiveFunctions()->IsSaving(ar); }
    static void SetDefaultInfoFunctions(GameStateInfoFunctions* f) { _defaultInfoFunctions = f; }

    // Same lazy-fallback pattern as DefaultArchiveFunctions — covers the
    // window between program load and ExpressExt.cpp's registration
    // constructor running.
    static GameStateInfoFunctions* DefaultInfoFunctions()
    {
        if (_defaultInfoFunctions)
            return _defaultInfoFunctions;
        static GameStateInfoFunctions fallback;
        return &fallback;
    }

    bool IdtfGoodName(const char* name) const;
    bool VarGoodName(const char* name) const;
    bool LValueGoodName(const char* name) const;
    GameValue VarGet(const char* name) const;
    bool VarReadOnly(const char* name) const;
    //! Create a variable and assign a value. A name starting with '_', or
    //! forceLocal, makes it local to the script scope; otherwise it is global.
    void VarSet(const char* name, GameValuePar value, bool readOnly = false, bool forceLocal = false);
    void VarSetLocal(const char* name, GameValuePar value, bool readOnly = false, bool forceLocal = false) const;
    //! create undefined variable in the innermost scope
    void VarLocal(const char* name);
    void VarDelete(const char* name);

    bool IsVisible(const GameVariable& var) const;

    void ShowDebug(const RString& content, int timeMs) const;
    void SetError(EvalError error, ...) const;
    void TypeError(GameType exp, GameType was, const char* name = nullptr) const;

    EvalError GetLastError() const;
    RString GetLastErrorText() const;
    int GetLastErrorPos() const;

    //! Set when a script error is fatal (test mode): loops stop iterating, so a
    //! failing while/for cannot spin the frame forever after the abort was requested.
    static inline bool s_abortLoops = false;
    //! Arma 2 content writes strings in single quotes too ('text'); OFP's scripts use
    //! the apostrophe in comments and SQS text, so it is a string delimiter only when set.
    static inline bool s_singleQuoteStrings = false;
    // Arma 3 content: {...} evaluates to a GameDataCode (typeName "CODE") instead of a
    // plain string.
    static inline bool s_codeValues = false;
    // A global variable that is not set: asked before nil is returned (Arma 3: the
    // functions of CfgFunctions are compiled when first used). Returns true and fills
    // `value` when the name stands for something; the value is then kept as the variable.
    static inline bool (*s_resolveGlobal)(const char* lowerName, GameValue& value) = nullptr;
    // Arma 1/2 semantics: an empty statement (a trailing ';' before the end of the code,
    // or ';;') does not replace the value of the statement before it. Arma 2's function
    // library returns values from code ending in "_this call f;\n".
    static inline bool s_emptyStatementKeepsValue = false;
    // breakTo / breakOut (Arma 2): the scope being broken to, and whether that scope is
    // left as well (breakOut). Set while a break unwinds; empty otherwise.
    static inline std::string s_breakTarget;
    static inline bool s_breakOut = false;
    void SetScopeName(const char* name) const;
    void BreakToScope(const char* name, bool out) const;
    // value breakOut "scope" (Arma 3): the scope left gives this value as its result.
    void BreakOutWithValue(const char* name, const GameValue& value) const;
    // A statement loop stopped by EvalBreak: true if this scope is the break's target
    // (the break is then over: the statement is skipped, or the scope left for breakOut).
    bool CatchBreak(bool& leaveScope) const;
    // Optional observer of every expression the evaluator starts (stall diagnostics in the
    // Arma 2 test mode); null when unused.
    static inline void (*s_traceExpression)(const char* expression) = nullptr;
    // Scheduled scripts that suspend anywhere (Arma 2): the number of suspensions so far,
    // so a while loop that sleeps is not cut at the unscheduled iteration limit. Null
    // when unused.
    static inline unsigned (*s_scheduledYields)() = nullptr;
    // Called once per while-loop iteration; a scheduled script gives the frame back when
    // its time slice is used up. Null when unused.
    static inline void (*s_scheduledTick)() = nullptr;

    // Evaluator contexts of a suspended script are taken off the shared stack while it
    // waits and put back when it resumes.
    int ContextLevel() const { return _actContext; }
    void DetachContextsAbove(int level, std::vector<Ref<GameEvaluator>>& out) const
    {
        out.clear();
        for (int i = level + 1; i <= _actContext; i++)
        {
            out.push_back(_contextStack[i]);
            _contextStack[i].Free();
        }
        if (_actContext > level)
        {
            _actContext = level;
            _e = _contextStack[_actContext];
        }
    }
    void AttachContexts(std::vector<Ref<GameEvaluator>>& in) const
    {
        for (Ref<GameEvaluator>& c : in)
        {
            if (_actContext + 1 >= MaxContexts)
                break;
            _contextStack[++_actContext] = c;
        }
        _e = _contextStack[_actContext];
        in.clear();
    }

    // evaluate an expression
    //! get actual context (started by BeginContext)
    GameVarSpace* GetContext() const;

    //! start context with given local variable space
    void BeginContext(GameVarSpace* vars) const;
    //! close context
    void EndContext() const;

    struct EvaluateTerm
    {
        char* terminationBy;
    };

    GameValue Evaluate(const char* expression) const;
    GameValue EvaluateMultiple(const char* expression, bool localOnly = false) const;
    bool EvaluateMultipleBool(const char* expression, bool localOnly = false) const;
    bool EvaluateBool(const char* expression) const;
    //! execute one assignment command
    bool PerformAssignment(bool localOnly = false);
    //! execute command or list of commands
    void Execute(const char* expression);

    void ShowError() const;

    // check syntax
    bool CheckEvaluate(const char* expression) const;
    bool CheckEvaluateBool(const char* expression) const;
    bool CheckExecute(const char* expression) const;

    // serialization support

    virtual GameData* CreateGameData(GameType type) const;
    virtual GameValue CreateGameValue(GameType type) const;
    virtual RString GetTypeName(GameType type) const;

#if !ACCESS_ONLY
    LSError Serialize(ParamArchive& ar) override;
    static GameState* CreateObject(ParamArchive& /*ar*/) { return new GameState(); }
#else
    virtual LSError Serialize(ParamArchive& ar) { return LSOK; }
#endif

    // diagnostic support
    const VarBankType& GetVariables() const { return _vars; }
    VarBankType& GetVariables() { return _vars; }
};

// Process-lifetime singleton - no global constructor and no exit-time
// destructor. GGameState owns evaluator context refs whose teardown can
// run after other engine shutdown has already advanced, so we keep it
// alive until process exit instead of destroying it during atexit.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"

inline GameState& GGameState()
{
    static GameState* instance = new GameState();
    return *instance;
}

#pragma clang diagnostic pop

// Lets call sites write `GGameState.X`, expanding it to the accessor call
// `GGameState().X`.
#define GGameState GGameState()

#endif
