#pragma once

#include <Poseidon/Foundation/Containers/TypeOpts.hpp>
#include <Poseidon/Foundation/Containers/ConstructTraitsModern.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>

#define CB_LOG_NEW_DELETE 0

#pragma warning(disable : 4200)

namespace Poseidon::Foundation
{
template <class Type>
class CompactBuffer
{
    // include RefCount functionality to avoid virtual functions table and delete

  private:
    // Reference count. ATOMIC, and it has to be: CompactBuffer<char> is the buffer
    // behind every RString, and therefore behind every interned RStringB. The
    // RStringB bank (RString.hpp) holds a std::mutex around the *map*, but the
    // string it hands back is refcounted OUTSIDE that lock -- every Ref<String>
    // copy, assignment and destructor, in whatever thread happens to hold the
    // string. Two threads interning the same name (two ShapeAdapter conversions of
    // one model on the preparer's workers, say) then race `++_count` / `--_count`,
    // lose an increment, and Release() reaches 0 while a live reference remains:
    // `delete[] (char*)this` on memory somebody still owns. The symptom is heap
    // corruption inside the allocator at an unrelated site some time later, which
    // is exactly what SIM-807 measured.
    //
    // This mirrors RefCount (Pointers.hpp), which was made atomic for the same
    // reason; CompactBuffer deliberately does NOT derive from it (no vtable), so it
    // did not inherit the fix. GCC __atomic builtins rather than std::atomic<int>
    // for the same reason as RefCount: no libatomic dependency.
    mutable int _count;

  public:
    int AddRef() const { return __atomic_add_fetch(&_count, 1, __ATOMIC_RELAXED); }
    int Release() const
    {
        // acq_rel so the decrement that reaches 0 happens-after every other thread's
        // release: their writes are visible before we free the buffer.
        int ret = __atomic_sub_fetch(&_count, 1, __ATOMIC_ACQ_REL);
        if (ret == 0)
        {
            Delete();
        }
        return ret;
    }
    int RefCounter() const { return __atomic_load_n(&_count, __ATOMIC_RELAXED); }

  private:
    int _size;
    Type _data[1];

  public:
    explicit CompactBuffer(int count)
    {
        _count = 0;
        _size = count;
        ::Poseidon::Foundation::ModernTraits<Type>::ConstructArray(_data, _size);
    }
    CompactBuffer(Type const* data, int count)
    {
        _count = 0;
        _size = count;
        ::Poseidon::Foundation::ModernTraits<Type>::CopyConstruct(_data, data, _size);
    }
    ~CompactBuffer() { ::Poseidon::Foundation::ModernTraits<Type>::DestructArray(_data, _size); }

  private: // disable copy
    void operator=(const CompactBuffer& src);
    CompactBuffer(const CompactBuffer& src);

#undef new
    // note: new and delete must never be used on CompactBuffer
    // Declared but not defined - will cause linker error if used
    static void* operator new(size_t size);
    // Provide stub to avoid linker errors from compiler-generated code
    static void operator delete(void* mem) { (void)mem; }

    // Allow placement new/delete
    static void* operator new(size_t size, void* placement)
    {
        (void)size;
        return placement;
    }
    static void operator delete(void* mem, void* placement)
    {
        (void)mem;
        (void)placement;
    }

#if ALLOC_DEBUGGER
#ifdef MFC_NEW
#define new DEBUG_NEW
#else
#define new debugNew
#endif
#endif

  public:
    Type* Data() { return _data; }
    const Type* Data() const { return _data; }
    int Size() const { return _size; }

    static CompactBuffer* New(int n)
    {
        PoseidonAssert(n > 0);
        int size = sizeof(CompactBuffer) - sizeof(Type) + sizeof(Type) * n;

        CompactBuffer* buffer;

// Temporarily disable debug new macro for raw allocation
#undef new
        buffer = (CompactBuffer*)new char[size];
#if ALLOC_DEBUGGER
#ifdef MFC_NEW
#define new DEBUG_NEW
#else
#define new debugNew
#endif
#endif

// placement new required
#undef new
        new (buffer) CompactBuffer(n);
#if ALLOC_DEBUGGER
#ifdef MFC_NEW
#define new DEBUG_NEW
#else
#define new debugNew
#endif
#endif

#if CB_LOG_NEW_DELETE
        LOG_DEBUG(Core, "New {:x}: Size {}", (uintptr_t)buffer, size);
#endif
        return buffer;
    }

    void Delete() const
    {
#if CB_LOG_NEW_DELETE
        LOG_DEBUG(Core, "Delete {:x}: Size {}", (uintptr_t)this,
                  _size * sizeof(Type) + sizeof(CompactBuffer) - sizeof(Type));
#endif
        this->~CompactBuffer();
        delete[] (char*)this;
    }

    static CompactBuffer* Copy(CompactBuffer* str)
    {
        int len = str->Size();
        PoseidonAssert(len > 0);
        CompactBuffer* buffer = (CompactBuffer*)new char[sizeof(CompactBuffer) - sizeof(Type) + sizeof(Type) * len];
#undef new
        new (buffer) CompactBuffer(str->Data(), len);
#if ALLOC_DEBUGGER
#ifdef MFC_NEW
#define new DEBUG_NEW
#else
#define new debugNew
#endif
#endif
        return buffer;
    }
    static CompactBuffer* Copy(CompactBuffer* str, int len)
    {
        if (len > str->Size())
        {
            len = str->Size();
        }
        PoseidonAssert(len > 0);
        int size = sizeof(CompactBuffer) - sizeof(Type) + sizeof(Type) * len;
        CompactBuffer* buffer = (CompactBuffer*)new char[size];
#undef new
        new (buffer) CompactBuffer(str->Data(), len);
#if ALLOC_DEBUGGER
#ifdef MFC_NEW
#define new DEBUG_NEW
#else
#define new debugNew
#endif
#endif
#if CB_LOG_NEW_DELETE
        LOG_DEBUG(Core, "Copy {:x}: Size {}", (uintptr_t)buffer, size);
#endif
        return buffer;
    }
    static CompactBuffer* Copy(Type const* data, int len)
    {
        PoseidonAssert(len > 0);
        int size = sizeof(CompactBuffer) - sizeof(Type) + sizeof(Type) * len;
        CompactBuffer* buffer = (CompactBuffer*)new char[size];
#undef new
        new (buffer) CompactBuffer(data, len);
#if ALLOC_DEBUGGER
#ifdef MFC_NEW
#define new DEBUG_NEW
#else
#define new debugNew
#endif
#endif
#if CB_LOG_NEW_DELETE
        LOG_DEBUG(Core, "Copy {:x}: Size {}", (uintptr_t)buffer, size);
#endif
        return buffer;
    }
};

typedef CompactBuffer<char> CompactString;

} // namespace Poseidon::Foundation


using ::Poseidon::Foundation::CompactBuffer;
using ::Poseidon::Foundation::CompactString;
