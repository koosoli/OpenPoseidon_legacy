#pragma once

#include <string.h>
#include <ctype.h>

#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/Memory/FastAlloc.hpp>

#include <Poseidon/Foundation/Containers/CompactBuf.hpp>
#include <Poseidon/Foundation/Containers/HashMap.hpp>
#include <mutex>

namespace Poseidon::Foundation
{
typedef CompactBuffer<char> String;

inline String* CreateString(const char* str, int len)
{
    String* string = String::New(len + 1);
    ::strncpy(string->Data(), str, len);
    string->Data()[len] = 0;
    return string;
}

inline String* CreateString(const char* str)
{
    int len = static_cast<int>(strlen(str)) + 1;
    return String::Copy(str, len);
}

inline String* CreateString(const char* a, const char* b)
{
    int len = static_cast<int>(strlen(a) + strlen(b)) + 1;
    String* string = String::New(len);
    ::strcpy(string->Data(), a);
    ::strcat(string->Data(), b);
    return string;
}

// Reference-counted, copy-on-write string. Identical strings share one
// allocation, split lazily on the first mutation.
class RString
{
  protected:
    Ref<String> _ref;

  public:
    RString() = default;
    RString(const char* text)
    {
        if (text)
        {
            _ref = CreateString(text);
        }
    }
    RString(const char* text, int len)
    {
        if (text)
        {
            _ref = CreateString(text, len);
        }
    }
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - Parameter order documented (a then b)
    RString(const char* a, const char* b) { _ref = CreateString(a, b); }
    // Null-terminated data; never null (returns "" when unset).
    const char* Data() const
    {
        if (_ref)
        {
            return _ref->Data();
        }
        else
        {
            return ""; // always return a valid string
        }
    }
    // Hash key for MapStringToClass: the string is its own key.
    __forceinline const char* GetKey() const { return Data(); }

    // Detach from any shared copy and return a writable pointer; null if unset.
    char* MutableData()
    {
        if (!_ref)
        {
            return nullptr;
        }
        MakeMutable();
        return _ref->Data();
    }
    __forceinline operator const char*() const { return Data(); }
    int GetLength() const { return _ref ? strlen(_ref->Data()) : 0; }
    int GetRefCount() const { return _ref->RefCounter(); }
    char operator[](int i) const { return Data()[i]; }

    // Copy-on-write: if the buffer is shared, split off a private copy.
    void MakeMutable()
    {
        if (_ref && _ref->RefCounter() > 1)
        {
            _ref = CreateString(_ref->Data());
        }
    }
    void Lower()
    {
        MakeMutable();
        if (_ref)
        {
            ::strlwr(_ref->Data());
        }
    }
    void Upper()
    {
        MakeMutable();
        if (_ref)
        {
            ::strupr(_ref->Data());
        }
    }
    // Substring [from, to). Indices are clamped to the string length.
    RString Substring(int from, int to) const
    {
        int len = GetLength();
        if (from > len)
        {
            from = len;
        }
        if (to > len)
        {
            to = len;
        }
        // whole string: share instead of copying
        if (from == 0 && to == len)
        {
            return *this;
        }
        return RString(Data() + from, to - from);
    }

    // Case-sensitive raw strcmp; prefer RStringS / RStringI for comparisons.
    bool operator==(const RString& with) const { return strcmp(*this, with) == 0; }
    bool operator!=(const RString& with) const { return strcmp(*this, with) != 0; }
};

// RString with a comparison policy (Compare functor).
template <class Compare>
class RStringT : public RString
{
  public:
    RStringT() = default;
    RStringT(const RString& text) : RString(text) {}
    RStringT(const char* text) : RString(text) {}
    RStringT(const char* text, int len) : RString(text, len) {}
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - Parameter order documented (a then b)
    RStringT(const char* a, const char* b) : RString(a, b) {}
    bool operator==(const RStringT& with) const
    {
        Compare cmp;
        return cmp(*this, with) == 0;
    }
    bool operator!=(const RStringT& with) const
    {
        Compare cmp;
        return cmp(*this, with) != 0;
    }

  private:
    void Lower();
    void Upper();

  public:
};

// case-sensitive comparison functor for RStringT
class CmpStrCS
{
  public:
    int operator()(const char* a, const char* b) { return strcmp(a, b); }
};

// case-insensitive comparison functor for RStringT
class CmpStrCI
{
  public:
    int operator()(const char* a, const char* b) { return strcmpi(a, b); }
};

typedef RStringT<CmpStrCS> RStringS; // case-sensitive
typedef RStringT<CmpStrCI> RStringI; // case-insensitive

template <>
struct MapClassTraits<RStringI>
{
    typedef const char* KeyType;
    static unsigned int CalculateHashValue(KeyType key)
    {
        unsigned int nHash = 0;
        while (*key)
        {
            nHash = nHash * 32 + nHash + tolower(*key++);
        }
        return nHash;
    }

    static int CmpKey(KeyType k1, KeyType k2) { return strcmpi(k1, k2); }

    static KeyType GetKey(const RStringI& item) { return item.GetKey(); }
};

// Interning pool: deduplicates RStrings so equal strings share one allocation.
template <class RStringType>
class RStringBankT
{
    // Note: keep strings sorted; maintain a first-letter index table (256 entries)
    typedef MapStringToClass<RStringType, AutoArray<RStringType>> RStringBankType;

    RStringBankType _bank;

    // Guards _bank. Interning used to be main-thread-only; the streamed-model
    // preparer is about to run ShapeAdapter conversion on workers, and every
    // RStringB they create goes through this bank. Individual Get/Add locking
    // would not be enough: the returned reference points INTO the map, which a
    // concurrent Add may rehash - so the public operations copy the result out
    // under the lock (GetOrAdd) or re-check liveness under it (Unregister).
    mutable std::mutex _mtx;

    // Items are reference-counted; an item dropping to a single reference is unused
    // and gets removed on Compact().

  public:
    RStringBankT();
    ~RStringBankT();
    void Clear();

    static bool IsNull(const RStringType& value) { return RStringBankType::IsNull(value); }
    static bool NotNull(const RStringType& value) { return RStringBankType::NotNull(value); }
    static RStringType& Null() { return RStringBankType::Null(); }

    // Look up or intern `t` in one critical section and return BY VALUE (the
    // copy shares the atomically refcounted String, so it stays valid after the
    // lock drops even if a concurrent Add rehashes the map).
    RStringType GetOrAdd(const char* t);

    void Unregister(const char* str);

    void Compact();
};

// Interned string: a function pointer reaches the singleton bank (avoids SIOF).
template <class RStringType, RStringBankT<RStringType>& (*GetBank)()>
class RStringBT : public RStringType
{
    typedef RStringBankT<RStringType> RStringBankType;

    void Free();
    void Copy(const RStringBT& src) { this->_ref = src._ref; }

  public:
    RStringBT() = default;
    RStringBT(RString str);
    RStringBT(RStringType str);
    RStringBT(const char* str);

    RStringBT(const RStringBT& src) { Copy(src); }
    void operator=(const RStringBT& src)
    {
        if (src._ref != this->_ref)
        {
            Free();
            Copy(src);
        }
    }
    ~RStringBT() { Free(); }

    bool operator==(const RStringBT& with) const { return this->_ref == with._ref; }
    bool operator!=(const RStringBT& with) const { return this->_ref != with._ref; }

};

#if 1
// Meyers Singleton for string bank - avoids SIOF (Static Initialization Order Fiasco)
typedef RStringBankT<RStringS> RStringBank;
inline RStringBank& GetStringBank()
{
    [[clang::no_destroy]] static RStringBank instance;
    return instance;
}
typedef RStringBT<RStringS, &GetStringBank> RStringB;
// Empty string accessor - avoids global constructor
inline const RStringB& RStringBEmpty()
{
    [[clang::no_destroy]] static RStringB instance("");
    return instance;
}
#endif

#if 1
// Meyers Singleton for case-insensitive string bank
typedef RStringBankT<RStringI> RStringBankI;
inline RStringBankI& GetStringBankI()
{
    [[clang::no_destroy]] static RStringBankI instance;
    return instance;
}
typedef RStringBT<RStringI, &GetStringBankI> RStringIB;
// Empty string accessor - avoids global constructor
inline const RStringIB& RStringIBEmpty()
{
    [[clang::no_destroy]] static RStringIB instance("");
    return instance;
}
#endif

// StringBank / StringBankI spell the singleton bank accessors as lvalues.
#define StringBank GetStringBank()
#define StringBankI GetStringBankI()

// RStringBEmpty / RStringIBEmpty are written as constants (no parens); the
// macros supply the call.
using ::Poseidon::Foundation::RStringBEmpty;
using ::Poseidon::Foundation::RStringIBEmpty;
#define RStringBEmpty RStringBEmpty()
#define RStringIBEmpty RStringIBEmpty()

template <class RStringType, RStringBankT<RStringType>& (*GetBank)()>
RStringBT<RStringType, GetBank>::RStringBT(const char* str)
{
    // search for string
    if (!str)
    {
        return;
    }
    RStringType interned = GetBank().GetOrAdd(str);
    PoseidonAssert(GetBank().NotNull(interned));
    RStringType::operator=(interned);
}

template <class RStringType, RStringBankT<RStringType>& (*GetBank)()>
RStringBT<RStringType, GetBank>::RStringBT(RString str)
{
    // search for string
    if (!str)
    {
        return;
    }
    RStringType interned = GetBank().GetOrAdd(str);
    PoseidonAssert(GetBank().NotNull(interned));
    RStringType::operator=(interned);
}

template <class RStringType, RStringBankT<RStringType>& (*GetBank)()>
RStringBT<RStringType, GetBank>::RStringBT(RStringType str)
{
    // search for string
    if (!str)
        return;
    RStringType interned = GetBank().GetOrAdd(str);
    PoseidonAssert(RStringBankType::NotNull(interned));
    RStringType::operator=(interned);
}

template <class RStringType, RStringBankT<RStringType>& (*GetBank)()>
void RStringBT<RStringType, GetBank>::Free()
{
    if (this->_ref && this->_ref->RefCounter() == 2)
    {
        GetBank().Unregister(*this);
    }
    this->_ref.Free();
}

template <class RStringType, class RStringBankType>
void CheckRefsWeak(RStringType& str, RStringBankType* bank, void* context)
{
    (void)bank;
    (void)context;
    if (str.GetRefCount() > 1)
    {
        LOG_DEBUG(Core, "Error: String {} late release ({})", (const char*)str, str.GetRefCount());
        // do not delete, bank will be cleared
    }
}

template <class RStringType, class RStringBankType>
void CheckRefsStrong(RStringType& str, RStringBankType* bank, void* context)
{
    (void)bank;
    (void)context;
    if (str.GetRefCount() > 0)
    {
        LOG_DEBUG(Core, "Warning: String {} late release ({})", (const char*)str, str.GetRefCount());
        // do not delete, bank will be cleared
    }
}

template <class RStringType>
RStringBankT<RStringType>::RStringBankT() = default;

template <class RStringType>
RStringBankT<RStringType>::~RStringBankT()
{
    LOG_DEBUG(Core, "RStringBank destruct");
    _bank.ForEach(CheckRefsWeak);
    _bank.Clear();
}

template <class RStringType>
void RStringBankT<RStringType>::Clear()
{
    std::lock_guard<std::mutex> lock(_mtx);
    LOG_DEBUG(Core, "RStringBank clear");
    _bank.ForEach(CheckRefsWeak);
    _bank.Clear();
}

template <class RStringType>
RStringType RStringBankT<RStringType>::GetOrAdd(const char* t)
{
    std::lock_guard<std::mutex> lock(_mtx);
    const RStringType& str = _bank[t];
    if (NotNull(str))
    {
        return str;
    }
    RStringType newstr(t);
    _bank.Add(newstr);
    return _bank[newstr];
}

template <class RStringType, class RStringBankType>
static void DeleteUnused(RStringType& str, RStringBankType* bank, void* context)
{
    (void)context;
    if (str.GetRefCount() == 1)
    {
        LOG_DEBUG(Core, "String {} not released", (const char*)str);
        bank->Remove(str);
        // ForEach will restart (this array)
    }
}

template <class RStringType>
void RStringBankT<RStringType>::Compact()
{
    std::lock_guard<std::mutex> lock(_mtx);
    _bank.ForEach(DeleteUnused);
}

#define RStringBVal const RStringB&
#define RStringIBVal const RStringIB&

// define static RStringB with lazy initialization to avoid SIOF
#define DEF_RSB(x)                   \
    inline const RStringB& RSB_##x() \
    {                                \
        static RStringB str_##x(#x); \
        return str_##x;              \
    }
#define RSB(x) RSB_##x()

#define DEF_RSBI(x)                    \
    inline const RStringIB& RSBI_##x() \
    {                                  \
        static RStringIB stri_##x(#x); \
        return stri_##x;               \
    }
#define RSBI(x) RSBI_##x()

#if 1
inline RString operator+(const RString& a, const RString& b)
{
    return RString(a, b);
}
#endif

template <class RStringType>
inline void RStringBankT<RStringType>::Unregister(const char* str)
{
    std::lock_guard<std::mutex> lock(_mtx);
    // The caller probed the refcount OUTSIDE the lock (RStringBT::Free sees
    // bank + self == 2). Another thread may have interned the same string in
    // between; re-check liveness here so a live entry is never removed.
    const RStringType& live = _bank[str];
    if (NotNull(live) && live.GetRefCount() > 2)
    {
        return;
    }
    _bank.Remove(str);
}

} // namespace Poseidon::Foundation


using ::Poseidon::Foundation::RString;
using ::Poseidon::Foundation::RStringS;
using ::Poseidon::Foundation::RStringI;
using ::Poseidon::Foundation::RStringB;
using ::Poseidon::Foundation::RStringIB;
using ::Poseidon::Foundation::RStringBank;
using ::Poseidon::Foundation::RStringBankI;
using ::Poseidon::Foundation::RStringBankT;
