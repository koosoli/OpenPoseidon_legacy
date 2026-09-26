#ifdef _MSC_VER
#pragma once
#endif

#ifndef _PARAM_FILE_HPP
#define _PARAM_FILE_HPP

#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>

#include <Poseidon/IO/PreprocC/IPreproc.hpp>
#include <Poseidon/Foundation/Containers/RStringArray.hpp>


class GameVarSpace;

namespace Poseidon
{
class SerializeBinStream;

class IParamArrayValue;

class ParamRawValue;

class PASumCalculator;

// Access mode for ParamFile entries: protects config against modification by the
// add-on mechanism.
enum ParamAccessMode
{
    PADefault = -1,    //! no set - any modifications enabled
    PAReadAndWrite,    //! any modifications enabled
    PAReadAndCreate,   //! only adding new class members is allowed
    PAReadOnly,        //! no modifications enabled
    PAReadOnlyVerified //! no modifications enabled, CRC test applied
};

class ParamEntry;
class ParamFile;

// Functor interface used to check whether an entry can be accessed.
class IParamVisibleTest
{
  public:
    virtual bool operator()(const ParamEntry& entry) = 0;
    // parent is guaranteed visible.
    virtual bool operator()(const ParamEntry& parent, const ParamEntry& entry) = 0;
};

extern IParamVisibleTest& DefaultAccess;

// Most common visibility policy: an entry is accessible if its owner is in the list.
class ParamClass; // forward declaration for use in ParamEntry

class ParamOwnerList : public IParamVisibleTest
{
    FindArrayRStringBCI _addons;

  public:
    void Clear() { _addons.Clear(); }
    void Add(RString addon)
    {
        addon.Lower();
        _addons.AddUnique((const char*)addon);
    }
    int GetSize() const { return _addons.Size(); }
    const RStringB& Get(int i) const { return _addons[i]; }
    bool operator()(const ParamEntry& parent, const ParamEntry& entry) override;
    bool operator()(const ParamEntry& entry) override;
};

class ParamEntry
{
    friend class ParamClass;

  protected:
    RStringB _name;

    void SetName(const RStringB& name) { _name = name; }

  public:
    explicit ParamEntry(const RStringB& name);
    const RStringB& GetName() const { return _name; }
    RString GetContext(const char* member = nullptr) const; // fully qualified name

    virtual bool IsClass() const { return false; }
    virtual const ParamClass* GetClassInterface() const { return nullptr; }
    virtual ParamClass* GetClassInterface() { return nullptr; }
    virtual bool IsArray() const { return false; }
    virtual bool IsTextValue() const { return false; }
    virtual bool IsFloatValue() const { return false; }
    virtual bool IsIntValue() const { return false; }
    virtual bool IsError() const { return false; }
    virtual ~ParamEntry();

    // Access control
    virtual void SetAccessMode(ParamAccessMode /*mode*/) {}
    virtual void SetAccessModeForAll(ParamAccessMode /*mode*/) {}
    virtual ParamAccessMode GetAccessMode() const { return PADefault; }
    //! owner - used to check if given entry can be accessed
    virtual const RStringB& GetOwner() const;
    virtual void SetOwner(RString owner, bool subentries = true);
    virtual bool CheckVisible(IParamVisibleTest& visible) const;

    // value conversions
    virtual operator RStringB() const;
    virtual operator float() const;
    virtual operator int() const;
    virtual operator bool() const;
    virtual int GetInt() const;
    virtual RStringB GetValueRaw() const;
    virtual RStringB GetValue() const;

    virtual void SetValue(const RStringB& string);
    virtual void SetValue(float val);
    virtual void SetValue(int val);
    virtual void SetFile(ParamFile* file) = 0;

    virtual void Add(const RStringB& name, const RStringB& val);
    virtual void Add(const RStringB& name, float val);
    virtual void Add(const RStringB& name, int val);
    virtual ParamClass* AddClass(const RStringB& name, bool guaranteedUnique = false);
    virtual ParamEntry* AddArray(const RStringB& name);
    virtual void Clear();
    virtual void Delete(const RStringB& name);

    virtual void AddValue(float val);
    virtual void AddValue(int val);
    virtual void AddValue(const RStringB& val);
    virtual IParamArrayValue* AddArrayValue();

    virtual int GetEntryCount() const;
    virtual const ParamEntry& GetEntry(int i) const;

    virtual const IParamArrayValue& operator[](int index) const;
    virtual void SetValue(int index, const RStringB& string);
    virtual void SetValue(int index, float val);
    virtual void SetValue(int index, int val);
    virtual int GetSize() const;

    virtual ParamEntry* FindEntryNoInheritance(const RStringB& name, IParamVisibleTest& visible = DefaultAccess) const;
    virtual ParamEntry* FindEntry(const RStringB& name, IParamVisibleTest& visible = DefaultAccess) const;
    virtual const ParamEntry& operator>>(const char* name) const;

    virtual void Save(QOStream& /*f*/, int /*indent*/) const {}

    virtual void SerializeBin(SerializeBinStream& f);
    //! compact unused memory - usefull when finished adding entries
    virtual void Compact();
    //! provide an estimation of how many class entries will be added
    virtual void ReserveEntries(int count);
    //! provide an estimation of how many array elements will be added
    virtual void ReserveArrayElements(int count);

    //! calculate checksum of entry and all its verified children
    virtual void CalculateCheckValue(PASumCalculator& sum) const = 0;
    //! check if entry or some of its children is verified protected
    virtual bool HasChecksum() const;

    template <class Type>
    Type ReadValue(const char* name, const Type& defVal) const
    {
        const ParamEntry* entry = FindEntry(name);
        if (!entry)
        {
            return defVal;
        }
        else
        {
            return *entry;
        }
    }
};

class IParamArrayValue : public RefCount
{
  public:
    virtual RStringB GetValue() const = 0;
    virtual RStringB GetValueRaw() const = 0;
    virtual float GetFloat() const = 0;
    virtual int GetInt() const = 0;

    virtual void SetValue(const RStringB& val) = 0;
    virtual void SetValue(float val) = 0;
    virtual void SetValue(int val) = 0;
    virtual void SetFile(ParamFile* file) = 0;

    virtual bool IsTextValue() const = 0;
    virtual bool IsFloatValue() const = 0;
    virtual bool IsIntValue() const = 0;
    virtual bool IsArrayValue() const = 0;

    virtual void Save(QOStream& f, int indent) const = 0;
    virtual void SerializeBin(SerializeBinStream& f) = 0;

    // some handy conversions using virtual functions
    operator float() const { return GetFloat(); }
    operator int() const { return GetInt(); }
    operator RStringB() const { return GetValue(); }
    operator bool() const { return GetInt() != 0; }

    // may be array of values
    virtual const IParamArrayValue* GetItem(int i) const = 0;
    virtual int GetItemCount() const = 0;
    virtual void CalculateCheckValue(PASumCalculator& sum) const = 0;

    const IParamArrayValue& operator[](int i) const { return *GetItem(i); }

    virtual void AddValue(float val) = 0;
    virtual void AddValue(int val) = 0;
    virtual void AddValue(const RStringB& val) = 0;
    virtual IParamArrayValue* AddArrayValue() = 0;
};

class ParamClass : public ParamEntry
{
    friend class ParamEntry;
    friend class ParamFile;

    FindArray<SRef<ParamEntry>> _entries;
    InitPtr<ParamClass> _base;

    InitPtr<ParamClass> _parent;
    //! access right for modification by merge
    ParamAccessMode _access;
    //! owner - can be used to determine if class can be accessed
    RStringB _owner;

  public:
    bool IsClass() const override { return true; }
    const ParamClass* GetClassInterface() const override { return this; }
    ParamClass* GetClassInterface() override { return this; }

    /// Re-parent this class to inherit `base`'s entries via the >> lookup. Lets a
    /// class built at runtime clone an existing resource class (e.g. cloning the
    /// Quit button to inject a Mods menu entry) and override only a few fields.
    void SetBase(const ParamClass* base) { _base = const_cast<ParamClass*>(base); }

    const ParamClass* GetClass(const RStringB& name) const;
    using ParamEntry::GetValue; // Bring all base class overloads into scope
    RStringB GetValue(const RStringB& name) const;
    RStringB GetValue(const RStringB& name, int i) const;

    const ParamClass* GetParent() const { return _parent; }

    void Parse(QIStream& f, ParamFile* file); // read until close bracket
    void Save(QOStream& f, int indent) const override;
    void SerializeBin(SerializeBinStream& f) override;
    void Compact() override;
    void CheckInheritedAccess();
    void ReserveEntries(int count) override;

    void Update(const ParamClass& source);

    ParamEntry* FindEntry(const RStringB& name, IParamVisibleTest& visible = DefaultAccess) const override;
    ParamEntry* FindEntryNoInheritance(const RStringB& name, IParamVisibleTest& visible = DefaultAccess) const override;
    const ParamEntry& operator>>(const char* name) const override;

    void Add(const RStringB& name, const RStringB& val) override;
    void Add(const RStringB& name, float val) override;
    void Add(const RStringB& name, int val) override;

    void Delete(const RStringB& name) override;

    ParamClass* AddClass(const RStringB& name, bool guaranteedUnique = false) override;
    ParamEntry* AddArray(const RStringB& name) override;

    void AccessDenied(const char* name);

    void SetAccessModeForAll(ParamAccessMode mode) override;
    void SetAccessMode(ParamAccessMode mode) override { _access = mode; }
    ParamAccessMode GetAccessMode() const override { return _access; }
    const RStringB& GetOwner() const override;
    void SetOwner(RString owner, bool subentries = true) override;
    bool CheckVisible(IParamVisibleTest& visible) const override;

    const ParamClass* GetFile() const;
    void SetFile(ParamFile* file) override;

    int GetEntryCount() const override { return _entries.Size(); }
    const ParamEntry& GetEntry(int i) const override { return *_entries[i]; }
    const char* GetBaseName() const { return _base.NotNull() ? _base->GetName() : nullptr; }
    // Presence is independent of the base's name (runtime bases may be unnamed).
    bool HasBase() const { return _base.NotNull(); }

    bool IsDerivedFrom(const ParamClass& parent) const;

    void CalculateCheckValue(PASumCalculator& sum) const override;
    bool HasChecksum() const override;

    const ParamClass* SelectClassForChecking(int index) const;
    int GetNumberOfClassesForChecking() const;

  protected:
    ParamClass();
    ParamClass(const RStringB& name);
    ~ParamClass() override;

    void NewEntry(ParamEntry* entry, bool guaranteedUnique = false);

    ParamEntry* Find(const RStringB& name, bool parent, bool base, IParamVisibleTest& visible) const;
    int FindIndex(const RStringB& name) const;

    void Diagnostics(int indent);

    USE_FAST_ALLOCATOR
};

// Expression-evaluator callbacks. The *Internal variants skip the game-state
// context init/deinit that the plain variants do.
class EvaluatorFunctions
{
  public:
    EvaluatorFunctions() = default;
    virtual ~EvaluatorFunctions() = default;

    virtual GameVarSpace* CreateVariables() { return nullptr; }
    virtual void DeleteVariables(GameVarSpace* /*vars*/) {}
    virtual void InitEvaluator(GameVarSpace* /*vars*/) {}
    virtual void DoneEvaluator() {}
    virtual GameVarSpace* LoadVariables(SerializeBinStream& f);
    virtual void SaveVariables(SerializeBinStream& f, GameVarSpace* vars);
    virtual float EvaluateFloat(const char* /*expr*/, GameVarSpace* /*vars*/) { return 0; }
    virtual float EvaluateFloatInternal(const char* /*expr*/) { return 0; }
    virtual RString EvaluateStringInternal(const char* expr) { return expr; }
    virtual void ExecuteInternal(const char* /*expr*/) {}
    virtual void VarSetFloatInternal(const char* /*name*/, float /*value*/, bool /*readOnly*/, bool /*forceLocal*/) {}
};

class LocalizeStringFunctions
{
  public:
    LocalizeStringFunctions() = default;
    virtual ~LocalizeStringFunctions() = default;

    virtual RString LocalizeString(const char* str) { return str; }
};

class CRCFunctions
{
  public:
    CRCFunctions() = default;
    virtual ~CRCFunctions() = default;

    virtual void Add(PASumCalculator& sum, const void* buffer, int size)
    {
        (void)sum;
        (void)buffer;
        (void)size;
    }
};

class ParamFile : public ParamClass
{
  protected:
    GameVarSpace* _vars;
    static EvaluatorFunctions* _defaultEvalFunctions;
    static PreprocessorFunctions* _defaultPreprocFunctions;
    static LocalizeStringFunctions* _defaultLocalizeStringFunctions;
    static CRCFunctions* _defaultCRCFunctions;

  public:
    ParamFile();
    ~ParamFile() override;

    GameVarSpace* GetVariables() { return _vars; }
    static void SetDefaultEvalFunctions(EvaluatorFunctions* f) { _defaultEvalFunctions = f; }
    static void SetDefaultPreprocFunctions(PreprocessorFunctions* f) { _defaultPreprocFunctions = f; }
    static void SetDefaultLocalizeStringFunctions(LocalizeStringFunctions* f) { _defaultLocalizeStringFunctions = f; }
    static void SetDefaultCRCFunctions(CRCFunctions* f) { _defaultCRCFunctions = f; }

    // Lazy-fallback accessors for the four registered-default slots. The slots are
    // constant-init nullptr (no dynamic initializer, no SIOF risk); if nothing has
    // registered by first access, fall back to a no-op base-class Meyer's singleton.
    // All in-class methods route through these accessors, so a missed Init*Defaults()
    // never dereferences a null pointer.
    static EvaluatorFunctions* DefaultEvalFunctions()
    {
        if (_defaultEvalFunctions)
            return _defaultEvalFunctions;
        static EvaluatorFunctions fallback;
        return &fallback;
    }
    static PreprocessorFunctions* DefaultPreprocFunctions()
    {
        if (_defaultPreprocFunctions)
            return _defaultPreprocFunctions;
        static PreprocessorFunctions fallback;
        return &fallback;
    }
    static LocalizeStringFunctions* DefaultLocalizeStringFunctions()
    {
        if (_defaultLocalizeStringFunctions)
            return _defaultLocalizeStringFunctions;
        static LocalizeStringFunctions fallback;
        return &fallback;
    }
    static CRCFunctions* DefaultCRCFunctions()
    {
        if (_defaultCRCFunctions)
            return _defaultCRCFunctions;
        static CRCFunctions fallback;
        return &fallback;
    }

    LSError Parse(const char* name);
    LSError Save(const char* name) const;
    void Parse(QIStream& f);
    void Save(QOStream& f, int ident) const override;

    LSError ParsePlainText(const char* name);
    void ParsePlainText(QIStream& f);

    bool ParseBin(const char* name);
    bool SaveBin(const char* name);

    bool ParseBinOrTxt(const char* name);
    bool ParseBin(QFBank& bank, const char* name);

    void SerializeBin(SerializeBinStream& f) override;

    void Clear() override;

    void Reload();

    GameVarSpace* CreateVariables() { return DefaultEvalFunctions()->CreateVariables(); }
    void DeleteVariables()
    {
        if (_vars)
        {
            DefaultEvalFunctions()->DeleteVariables(_vars);
            _vars = nullptr;
        }
    }
    void InitEvaluator() { DefaultEvalFunctions()->InitEvaluator(_vars); }
    void DoneEvaluator() { DefaultEvalFunctions()->DoneEvaluator(); }
    void LoadVariables(SerializeBinStream& f)
    {
        DeleteVariables();
        _vars = DefaultEvalFunctions()->LoadVariables(f);
    }
    void SaveVariables(SerializeBinStream& f) { DefaultEvalFunctions()->SaveVariables(f, _vars); }
    float EvaluateFloat(const char* expr) { return DefaultEvalFunctions()->EvaluateFloat(expr, _vars); }
    float EvaluateFloatInternal(const char* expr) { return DefaultEvalFunctions()->EvaluateFloatInternal(expr); }
    RString EvaluateStringInternal(const char* expr) { return DefaultEvalFunctions()->EvaluateStringInternal(expr); }
    void ExecuteInternal(const char* expr) { DefaultEvalFunctions()->ExecuteInternal(expr); }
    void VarSetFloatInternal(const char* name, float value, bool readOnly = false, bool forceLocal = false)
    {
        DefaultEvalFunctions()->VarSetFloatInternal(name, value, readOnly, forceLocal);
    }

    bool Preprocess(QOStream& out, const char* name) { return DefaultPreprocFunctions()->Preprocess(out, name); }

    RString LocalizeString(const char* str) { return DefaultLocalizeStringFunctions()->LocalizeString(str); }

    static void AddCRC(PASumCalculator& sum, const void* buffer, int size)
    {
        DefaultCRCFunctions()->Add(sum, buffer, size);
    }

    USE_FAST_ALLOCATOR
};

} // namespace Poseidon

using Poseidon::ParamEntry;
using Poseidon::ParamClass;
using Poseidon::ParamFile;
using Poseidon::EvaluatorFunctions;
using Poseidon::PASumCalculator;
using Poseidon::CRCFunctions;
using Poseidon::IParamArrayValue;
using Poseidon::IParamVisibleTest;
using Poseidon::SerializeBinStream;
using Poseidon::PADefault;
using Poseidon::PAReadAndWrite;
using Poseidon::PAReadAndCreate;
using Poseidon::PAReadOnly;
using Poseidon::PAReadOnlyVerified;
using Poseidon::LocalizeStringFunctions;
using Poseidon::DefaultAccess;
using Poseidon::ParamOwnerList;

#endif
